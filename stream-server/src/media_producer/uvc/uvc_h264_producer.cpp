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

#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace media {
    namespace {

        std::string AvError(int error) {
            char text[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(error, text, sizeof(text));
            return text;
        }

        AVPixelFormat NormalizeJpegPixelFormat(AVPixelFormat format) {
            switch (format) {
            case AV_PIX_FMT_YUVJ420P:
                return AV_PIX_FMT_YUV420P;
            case AV_PIX_FMT_YUVJ422P:
                return AV_PIX_FMT_YUV422P;
            case AV_PIX_FMT_YUVJ444P:
                return AV_PIX_FMT_YUV444P;
            case AV_PIX_FMT_YUVJ440P:
                return AV_PIX_FMT_YUV440P;
            default:
                return format;
            }
        }

    } // namespace

    struct UvcH264Producer::Impl {
        struct PendingFrame {
            uvc::UvcFramePtr frame;
            std::chrono::steady_clock::time_point enqueued_at;
        };

        struct MailboxStatistics {
            uint64_t dropped_frames = 0;
            uint64_t dequeued_frames = 0;
            size_t current_depth = 0;
            size_t max_depth = 0;
            double average_wait_ms = 0.0;
            double max_wait_ms = 0.0;
        };

        explicit Impl(const UvcH264Config &producer_config) : config(producer_config), capture(config.capture) {
            capture.SetFrameCallback([this](uvc::UvcFramePtr frame) {
                if (accept_frames.load() && frame) {
                    QueueFrame(std::move(frame));
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
            StopProcessing();
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

        void ResetStatistics() {
            frames_sent = 0;
            decode_errors = 0;
            send_errors = 0;
            decode_time_ms = 0.0;
            max_decode_time_ms = 0.0;
            sws_time_ms = 0.0;
            max_sws_time_ms = 0.0;
            mb_get_time_ms = 0.0;
            max_mb_get_time_ms = 0.0;
            flush_time_ms = 0.0;
            max_flush_time_ms = 0.0;
            venc_send_time_ms = 0.0;
            max_venc_send_time_ms = 0.0;
            total_time_ms = 0.0;
            max_total_time_ms = 0.0;
            statistics_started = {};
        }

        bool StartProcessing() {
            if (processing_thread.joinable()) {
                StopProcessing();
            }
            {
                std::lock_guard<std::mutex> lock(mailbox_mutex);
                pending_frame.reset();
                processing_running = true;
                mailbox_dropped_frames = 0;
                mailbox_dequeued_frames = 0;
                mailbox_max_depth = 0;
                mailbox_wait_time_ms = 0.0;
                mailbox_max_wait_ms = 0.0;
            }
            try {
                processing_thread = std::thread(&Impl::ProcessingLoop, this);
            } catch (const std::exception &error) {
                std::lock_guard<std::mutex> lock(mailbox_mutex);
                processing_running = false;
                LOG_ERROR("Failed to create UVC H.264 processing thread: {}", error.what());
                return false;
            }
            LOG_INFO("UVC H.264 processing thread started with latest-frame mailbox capacity 1");
            return true;
        }

        void StopProcessing() {
            const bool had_thread = processing_thread.joinable();
            {
                std::lock_guard<std::mutex> lock(mailbox_mutex);
                processing_running = false;
                pending_frame.reset();
            }
            mailbox_condition.notify_all();
            if (had_thread) {
                processing_thread.join();
                LOG_INFO("UVC H.264 processing thread stopped");
            }
        }

        void QueueFrame(uvc::UvcFramePtr frame) {
            {
                std::lock_guard<std::mutex> lock(mailbox_mutex);
                if (!processing_running) {
                    return;
                }
                if (pending_frame) {
                    ++mailbox_dropped_frames;
                }
                pending_frame = PendingFrame{std::move(frame), std::chrono::steady_clock::now()};
                mailbox_max_depth = std::max<size_t>(mailbox_max_depth, 1);
            }
            mailbox_condition.notify_one();
        }

        void ProcessingLoop() {
            while (true) {
                PendingFrame queued_frame;
                {
                    std::unique_lock<std::mutex> lock(mailbox_mutex);
                    mailbox_condition.wait(lock, [this] { return !processing_running || pending_frame.has_value(); });
                    if (!processing_running) {
                        break;
                    }
                    queued_frame = std::move(*pending_frame);
                    pending_frame.reset();
                    const double wait_ms = std::chrono::duration<double, std::milli>(
                                                   std::chrono::steady_clock::now() - queued_frame.enqueued_at)
                                                   .count();
                    ++mailbox_dequeued_frames;
                    mailbox_wait_time_ms += wait_ms;
                    mailbox_max_wait_ms = std::max(mailbox_max_wait_ms, wait_ms);
                }

                try {
                    if (queued_frame.frame) {
                        EncodeFrame(*queued_frame.frame);
                    }
                } catch (const std::exception &error) {
                    LOG_ERROR("UVC H.264 processing threw an exception: {}", error.what());
                } catch (...) {
                    LOG_ERROR("UVC H.264 processing threw an unknown exception");
                }
            }
        }

        MailboxStatistics GetMailboxStatistics() {
            std::lock_guard<std::mutex> lock(mailbox_mutex);
            MailboxStatistics statistics;
            statistics.dropped_frames = mailbox_dropped_frames;
            statistics.dequeued_frames = mailbox_dequeued_frames;
            statistics.current_depth = pending_frame ? 1 : 0;
            statistics.max_depth = mailbox_max_depth;
            statistics.average_wait_ms = mailbox_dequeued_frames > 0
                                                 ? mailbox_wait_time_ms / mailbox_dequeued_frames
                                                 : 0.0;
            statistics.max_wait_ms = mailbox_max_wait_ms;
            return statistics;
        }

        void EncodeFrame(const uvc::UvcFrame &input) {
            if (input.data.empty() || input.data.size() > INT_MAX) {
                return;
            }

            const auto frame_started = std::chrono::steady_clock::now();
            const auto decode_started = frame_started;
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
            const auto decode_finished = std::chrono::steady_clock::now();
            const double decode_ms =
                    std::chrono::duration<double, std::milli>(decode_finished - decode_started).count();

            const auto mb_get_started = std::chrono::steady_clock::now();
            MB_BLK block = RK_MPI_MB_GetMB(input_pool, buffer_layout.u32MBSize, RK_TRUE);
            const auto mb_get_finished = std::chrono::steady_clock::now();
            if (block == MB_INVALID_HANDLE) {
                ++send_errors;
                LOG_ERROR("Failed to acquire NV12 input block for VENC");
                return;
            }
            const double mb_get_ms =
                    std::chrono::duration<double, std::milli>(mb_get_finished - mb_get_started).count();

            auto *base = static_cast<uint8_t *>(RK_MPI_MB_Handle2VirAddr(block));
            if (!base) {
                ++send_errors;
                RK_MPI_MB_ReleaseMB(block);
                LOG_ERROR("Failed to map NV12 input block");
                return;
            }

            const auto sws_started = std::chrono::steady_clock::now();
            const auto source_pixel_format =
                    NormalizeJpegPixelFormat(static_cast<AVPixelFormat>(decoded_frame->format));
            const bool source_full_range = decoded_frame->color_range != AVCOL_RANGE_MPEG;
            auto *previous_sws_context = sws_context;
            sws_context = sws_getCachedContext(
                    sws_context, decoded_frame->width, decoded_frame->height,
                    source_pixel_format, config.capture.width, config.capture.height,
                    AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws_context) {
                ++decode_errors;
                RK_MPI_MB_ReleaseMB(block);
                LOG_ERROR("Failed to create MJPEG-to-NV12 conversion context");
                return;
            }
            if (sws_context != previous_sws_context || source_pixel_format != configured_source_pixel_format ||
                source_full_range != configured_source_full_range) {
                const int *coefficients = sws_getCoefficients(SWS_CS_ITU601);
                result = sws_setColorspaceDetails(sws_context, coefficients, source_full_range ? 1 : 0,
                                                  coefficients, 0, 0, 1 << 16, 1 << 16);
                if (result < 0) {
                    ++decode_errors;
                    RK_MPI_MB_ReleaseMB(block);
                    LOG_ERROR("Failed to configure MJPEG-to-NV12 BT.601 color range");
                    return;
                }
                configured_source_pixel_format = source_pixel_format;
                configured_source_full_range = source_full_range;
                LOG_INFO("Configured MJPEG-to-NV12 colorspace: source_format={}, source_range={}, "
                         "matrix=BT.601, destination_range=limited",
                         static_cast<int>(source_pixel_format), source_full_range ? "full" : "limited");
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
            const auto sws_finished = std::chrono::steady_clock::now();
            const double sws_ms =
                    std::chrono::duration<double, std::milli>(sws_finished - sws_started).count();

            const auto flush_started = std::chrono::steady_clock::now();
            RK_MPI_SYS_MmzFlushCache(block, RK_FALSE);
            const auto flush_finished = std::chrono::steady_clock::now();
            const double flush_ms =
                    std::chrono::duration<double, std::milli>(flush_finished - flush_started).count();
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

            const auto venc_send_started = std::chrono::steady_clock::now();
            result = RK_MPI_VENC_SendFrame(config.venc_channel, &frame, 1000);
            const auto venc_send_finished = std::chrono::steady_clock::now();
            RK_MPI_MB_ReleaseMB(block);
            if (result != RK_SUCCESS) {
                ++send_errors;
                LOG_WARN("RK_MPI_VENC_SendFrame failed for UVC frame {}: {:#x}", input.sequence, result);
                return;
            }

            const double venc_send_ms =
                    std::chrono::duration<double, std::milli>(venc_send_finished - venc_send_started).count();
            const double total_ms =
                    std::chrono::duration<double, std::milli>(venc_send_finished - frame_started).count();
            decode_time_ms += decode_ms;
            max_decode_time_ms = std::max(max_decode_time_ms, decode_ms);
            sws_time_ms += sws_ms;
            max_sws_time_ms = std::max(max_sws_time_ms, sws_ms);
            mb_get_time_ms += mb_get_ms;
            max_mb_get_time_ms = std::max(max_mb_get_time_ms, mb_get_ms);
            flush_time_ms += flush_ms;
            max_flush_time_ms = std::max(max_flush_time_ms, flush_ms);
            venc_send_time_ms += venc_send_ms;
            max_venc_send_time_ms = std::max(max_venc_send_time_ms, venc_send_ms);
            total_time_ms += total_ms;
            max_total_time_ms = std::max(max_total_time_ms, total_ms);

            ++frames_sent;
            const auto now = std::chrono::steady_clock::now();
            if (frames_sent == 1) {
                statistics_started = now;
            } else if (frames_sent % 100 == 0) {
                const double seconds = std::chrono::duration<double>(now - statistics_started).count();
                const auto mailbox_statistics = GetMailboxStatistics();
                LOG_INFO("UVC H.264 input: frames={}, fps={:.2f}, "
                         "decode_ms(avg/max)={:.2f}/{:.2f}, sws_ms(avg/max)={:.2f}/{:.2f}, "
                         "mb_get_ms(avg/max)={:.3f}/{:.3f}, flush_ms(avg/max)={:.3f}/{:.3f}, "
                         "venc_send_ms(avg/max)={:.2f}/{:.2f}, total_ms(avg/max)={:.2f}/{:.2f}, "
                         "decode_errors={}, send_errors={}, mailbox_dequeued={}, mailbox_drops={}, "
                         "mailbox_depth(current/max)={}/{}, "
                         "mailbox_wait_ms(avg/max)={:.2f}/{:.2f}",
                         frames_sent, seconds > 0.0 ? (frames_sent - 1) / seconds : 0.0,
                         decode_time_ms / frames_sent, max_decode_time_ms, sws_time_ms / frames_sent,
                         max_sws_time_ms, mb_get_time_ms / frames_sent, max_mb_get_time_ms,
                         flush_time_ms / frames_sent, max_flush_time_ms, venc_send_time_ms / frames_sent,
                         max_venc_send_time_ms, total_time_ms / frames_sent, max_total_time_ms,
                         decode_errors, send_errors, mailbox_statistics.dequeued_frames,
                         mailbox_statistics.dropped_frames,
                         mailbox_statistics.current_depth, mailbox_statistics.max_depth,
                         mailbox_statistics.average_wait_ms, mailbox_statistics.max_wait_ms);
            }
        }

        UvcH264Config config;
        uvc::UvcProducer capture;
        EncodedStreamDispatcher dispatcher;

        AVCodecContext *decoder = nullptr;
        AVFrame *decoded_frame = nullptr;
        SwsContext *sws_context = nullptr;
        AVPixelFormat configured_source_pixel_format = AV_PIX_FMT_NONE;
        bool configured_source_full_range = false;

        MB_POOL input_pool = MB_INVALID_POOLID;
        MB_PIC_CAL_S buffer_layout{};
        bool mpi_initialized = false;
        bool venc_initialized = false;
        std::atomic<bool> accept_frames{false};

        std::mutex mailbox_mutex;
        std::condition_variable mailbox_condition;
        std::optional<PendingFrame> pending_frame;
        std::thread processing_thread;
        bool processing_running = false;
        uint64_t mailbox_dropped_frames = 0;
        uint64_t mailbox_dequeued_frames = 0;
        size_t mailbox_max_depth = 0;
        double mailbox_wait_time_ms = 0.0;
        double mailbox_max_wait_ms = 0.0;

        uint64_t frames_sent = 0;
        uint64_t decode_errors = 0;
        uint64_t send_errors = 0;
        double decode_time_ms = 0.0;
        double max_decode_time_ms = 0.0;
        double sws_time_ms = 0.0;
        double max_sws_time_ms = 0.0;
        double mb_get_time_ms = 0.0;
        double max_mb_get_time_ms = 0.0;
        double flush_time_ms = 0.0;
        double max_flush_time_ms = 0.0;
        double venc_send_time_ms = 0.0;
        double max_venc_send_time_ms = 0.0;
        double total_time_ms = 0.0;
        double max_total_time_ms = 0.0;
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
        impl_->ResetStatistics();
        if (!impl_->dispatcher.Start(config_.venc_channel)) {
            return false;
        }
        if (!impl_->StartProcessing()) {
            impl_->dispatcher.Stop();
            return false;
        }
        impl_->accept_frames.store(true);
        if (!impl_->capture.Start()) {
            impl_->accept_frames.store(false);
            impl_->StopProcessing();
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
        impl_->StopProcessing();
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
