#define LOG_TAG "UVC-H264"

#include "uvc_h264_producer.h"
#include "../encoded_stream_dispatcher.h"
#include "../rk_venc_config.h"
#include "common/logger.h"
#include "uvc_producer.h"

#include "rk_mpi_cal.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <climits>
#include <cstring>
#include <string>
#include <utility>

namespace media {
    namespace {

        std::string AvError(int error) {
            char text[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(error, text, sizeof(text));
            return text;
        }

    } // namespace

    struct UvcH264Producer::Impl {
        explicit Impl(const UvcH264Config &producer_config) : config(producer_config), capture(config.capture) {
            capture.SetFrameCallback([this](uvc::UvcFramePtr frame) {
                if (accept_frames.load() && frame) {
                    EncodeFrame(*frame);
                }
            });
        }

        int InitDecoder() {
            const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
            if (!codec) {
                LOG_ERROR("FFmpeg MJPEG decoder is unavailable");
                return -1;
            }
            decoder = avcodec_alloc_context3(codec);
            decoded_frame = av_frame_alloc();
            if (!decoder || !decoded_frame) {
                LOG_ERROR("Failed to allocate FFmpeg MJPEG decoder resources");
                return -1;
            }
            decoder->thread_count = 1;
            const int result = avcodec_open2(decoder, codec, nullptr);
            if (result < 0) {
                LOG_ERROR("Failed to open FFmpeg MJPEG decoder: {}", AvError(result));
                return -1;
            }
            LOG_INFO("FFmpeg MJPEG decoder initialized: {}", codec->name);
            return 0;
        }

        int InitMpi() {
            RK_S32 result = RK_MPI_SYS_Init();
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_SYS_Init failed: {:#x}", result);
                return -1;
            }
            mpi_initialized = true;

            PIC_BUF_ATTR_S picture{};
            picture.u32Width = config.capture.width;
            picture.u32Height = config.capture.height;
            picture.enCompMode = COMPRESS_MODE_NONE;
            picture.enPixelFormat = RK_FMT_YUV420SP;
            result = RK_MPI_CAL_COMM_GetPicBufferSize(&picture, &buffer_layout);
            if (result != RK_SUCCESS) {
                LOG_ERROR("Failed to calculate aligned NV12 buffer size: {:#x}", result);
                return -1;
            }

            MB_POOL_CONFIG_S pool_config{};
            pool_config.u64MBSize = buffer_layout.u32MBSize;
            pool_config.u32MBCnt = config.input_buffer_count;
            pool_config.enRemapMode = MB_REMAP_MODE_CACHED;
            pool_config.enAllocType = MB_ALLOC_TYPE_DMA;
            pool_config.enDmaType = MB_DMA_TYPE_CMA;
            pool_config.bPreAlloc = RK_TRUE;
            input_pool = RK_MPI_MB_CreatePool(&pool_config);
            if (input_pool == MB_INVALID_POOLID) {
                LOG_ERROR("Failed to create VENC NV12 input pool ({} buffers, {} bytes each)",
                          config.input_buffer_count, buffer_layout.u32MBSize);
                return -1;
            }

            result = InitH264Venc(config.venc_channel, config.capture.width, config.capture.height,
                                  config.bitrate_kbps, config.capture.fps, config.gop);
            if (result != RK_SUCCESS) {
                LOG_ERROR("Failed to initialize H.264 VENC channel {}: {:#x}", config.venc_channel, result);
                return -1;
            }
            venc_initialized = true;
            LOG_INFO("RKMPI VENC initialized: channel={}, {}x{}, bitrate={} kbps, input={} bytes",
                     config.venc_channel, config.capture.width, config.capture.height, config.bitrate_kbps,
                     buffer_layout.u32MBSize);
            return 0;
        }

        void Cleanup() {
            accept_frames.store(false);
            capture.Deinit();
            dispatcher.Stop();

            if (venc_initialized) {
                RK_MPI_VENC_StopRecvFrame(config.venc_channel);
                RK_MPI_VENC_DestroyChn(config.venc_channel);
                venc_initialized = false;
            }
            if (input_pool != MB_INVALID_POOLID) {
                RK_MPI_MB_DestroyPool(input_pool);
                input_pool = MB_INVALID_POOLID;
            }
            if (mpi_initialized) {
                RK_MPI_SYS_Exit();
                mpi_initialized = false;
            }
            if (sws_context) {
                sws_freeContext(sws_context);
                sws_context = nullptr;
            }
            if (decoded_frame) {
                av_frame_free(&decoded_frame);
            }
            if (decoder) {
                avcodec_free_context(&decoder);
            }
        }

        void EncodeFrame(const uvc::UvcFrame &input) {
            if (input.data.empty() || input.data.size() > INT_MAX) {
                return;
            }

            AVPacket packet{};
            packet.data = const_cast<uint8_t *>(input.data.data());
            packet.size = static_cast<int>(input.data.size());
            int result = avcodec_send_packet(decoder, &packet);
            if (result < 0) {
                ++decode_errors;
                LOG_WARN("avcodec_send_packet failed for UVC frame {}: {}", input.sequence, AvError(result));
                return;
            }

            av_frame_unref(decoded_frame);
            result = avcodec_receive_frame(decoder, decoded_frame);
            if (result < 0) {
                ++decode_errors;
                LOG_WARN("avcodec_receive_frame failed for UVC frame {}: {}", input.sequence, AvError(result));
                return;
            }
            if (decoded_frame->width != static_cast<int>(config.capture.width) ||
                decoded_frame->height != static_cast<int>(config.capture.height)) {
                ++decode_errors;
                LOG_WARN("Unexpected decoded MJPEG size: {}x{}", decoded_frame->width, decoded_frame->height);
                return;
            }

            MB_BLK block = RK_MPI_MB_GetMB(input_pool, buffer_layout.u32MBSize, RK_TRUE);
            if (block == MB_INVALID_HANDLE) {
                ++send_errors;
                LOG_ERROR("Failed to acquire NV12 input block for VENC");
                return;
            }

            auto *base = static_cast<uint8_t *>(RK_MPI_MB_Handle2VirAddr(block));
            if (!base) {
                ++send_errors;
                RK_MPI_MB_ReleaseMB(block);
                LOG_ERROR("Failed to map NV12 input block");
                return;
            }

            sws_context = sws_getCachedContext(
                    sws_context, decoded_frame->width, decoded_frame->height,
                    static_cast<AVPixelFormat>(decoded_frame->format), config.capture.width, config.capture.height,
                    AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws_context) {
                ++decode_errors;
                RK_MPI_MB_ReleaseMB(block);
                LOG_ERROR("Failed to create MJPEG-to-NV12 conversion context");
                return;
            }

            const int stride = static_cast<int>(
                    RK_MPI_CAL_COMM_GetHorStride(buffer_layout.u32VirWidth, RK_FMT_YUV420SP));
            uint8_t *destination[4] = {
                    base,
                    base + static_cast<size_t>(stride) * buffer_layout.u32VirHeight,
                    nullptr,
                    nullptr,
            };
            int destination_stride[4] = {stride, stride, 0, 0};
            const int rows = sws_scale(sws_context, decoded_frame->data, decoded_frame->linesize, 0,
                                       decoded_frame->height, destination, destination_stride);
            if (rows != static_cast<int>(config.capture.height)) {
                ++decode_errors;
                RK_MPI_MB_ReleaseMB(block);
                LOG_WARN("MJPEG-to-NV12 conversion returned {} rows", rows);
                return;
            }

            RK_MPI_SYS_MmzFlushCache(block, RK_FALSE);
            VIDEO_FRAME_INFO_S frame{};
            frame.stVFrame.pMbBlk = block;
            frame.stVFrame.u32Width = config.capture.width;
            frame.stVFrame.u32Height = config.capture.height;
            frame.stVFrame.u32VirWidth = buffer_layout.u32VirWidth;
            frame.stVFrame.u32VirHeight = buffer_layout.u32VirHeight;
            frame.stVFrame.enField = VIDEO_FIELD_FRAME;
            frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
            frame.stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
            frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
            frame.stVFrame.enDynamicRange = DYNAMIC_RANGE_SDR8;
            frame.stVFrame.enColorGamut = COLOR_GAMUT_BT601;
            frame.stVFrame.u32TimeRef = input.sequence;
            frame.stVFrame.u64PTS = input.timestamp_us;

            result = RK_MPI_VENC_SendFrame(config.venc_channel, &frame, 1000);
            RK_MPI_MB_ReleaseMB(block);
            if (result != RK_SUCCESS) {
                ++send_errors;
                LOG_WARN("RK_MPI_VENC_SendFrame failed for UVC frame {}: {:#x}", input.sequence, result);
                return;
            }

            ++frames_sent;
            const auto now = std::chrono::steady_clock::now();
            if (frames_sent == 1) {
                statistics_started = now;
            } else if (frames_sent % 100 == 0) {
                const double seconds = std::chrono::duration<double>(now - statistics_started).count();
                LOG_INFO("UVC H.264 input: {} frames, {:.2f} fps, decode_errors={}, send_errors={}", frames_sent,
                         seconds > 0.0 ? (frames_sent - 1) / seconds : 0.0, decode_errors, send_errors);
            }
        }

        UvcH264Config config;
        uvc::UvcProducer capture;
        EncodedStreamDispatcher dispatcher;

        AVCodecContext *decoder = nullptr;
        AVFrame *decoded_frame = nullptr;
        SwsContext *sws_context = nullptr;

        MB_POOL input_pool = MB_INVALID_POOLID;
        MB_PIC_CAL_S buffer_layout{};
        bool mpi_initialized = false;
        bool venc_initialized = false;
        std::atomic<bool> accept_frames{false};

        uint64_t frames_sent = 0;
        uint64_t decode_errors = 0;
        uint64_t send_errors = 0;
        std::chrono::steady_clock::time_point statistics_started;
    };

    UvcH264Producer::UvcH264Producer(UvcH264Config config) :
        config_(std::move(config)),
        shared_config_{static_cast<int>(config_.capture.fps), config_.bitrate_kbps},
        impl_(std::make_unique<Impl>(config_)) {}

    UvcH264Producer::~UvcH264Producer() { Deinit(); }

    int UvcH264Producer::Init() {
        if (initialized_.load()) {
            return 0;
        }
        if (config_.input_buffer_count < 2 || config_.venc_channel < 0 || config_.bitrate_kbps <= 0) {
            LOG_ERROR("Invalid UVC H.264 configuration");
            return -1;
        }
        if (impl_->capture.Init() != 0 || impl_->InitDecoder() != 0 || impl_->InitMpi() != 0) {
            impl_->Cleanup();
            return -1;
        }
        initialized_.store(true);
        LOG_INFO("UVC H.264 producer initialized: {}x{} @ {} fps", config_.capture.width,
                 config_.capture.height, config_.capture.fps);
        return 0;
    }

    int UvcH264Producer::Deinit() {
        Stop();
        impl_->Cleanup();
        initialized_.store(false);
        return 0;
    }

    bool UvcH264Producer::Start() {
        if (!initialized_.load()) {
            LOG_ERROR("UVC H.264 producer is not initialized");
            return false;
        }
        if (running_.load()) {
            return true;
        }
        if (!impl_->dispatcher.Start(config_.venc_channel)) {
            return false;
        }
        impl_->accept_frames.store(true);
        if (!impl_->capture.Start()) {
            impl_->accept_frames.store(false);
            impl_->dispatcher.Stop();
            return false;
        }
        running_.store(true);
        LOG_INFO("UVC H.264 producer started");
        return true;
    }

    void UvcH264Producer::Stop() {
        impl_->accept_frames.store(false);
        impl_->capture.Stop();
        impl_->dispatcher.Stop();
        if (running_.exchange(false)) {
            LOG_INFO("UVC H.264 producer stopped");
        }
    }

    void UvcH264Producer::RegisterStreamConsumer(const std::string &name, StreamCallback callback,
                                                 StreamConsumerType type, int queue_size) {
        (void) queue_size;
        impl_->dispatcher.RegisterConsumer(name, std::move(callback), type);
    }

    void UvcH264Producer::ClearStreamConsumers() { impl_->dispatcher.ClearConsumers(); }

    std::unique_ptr<IMediaProducer> CreateUvcH264Producer(const UvcH264Config &config) {
        return std::make_unique<UvcH264Producer>(config);
    }

} // namespace media
