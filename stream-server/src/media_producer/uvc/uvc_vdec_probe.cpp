#define LOG_TAG "UVCVdecProbe"

#include "uvc_producer.h"
#include "common/logger.h"

#include "rk_mpi_cal.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vdec.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace {

    struct Options {
        std::string device;
        std::string output = "/tmp/uvc_vdec_first.yuv";
        uint32_t frames = 300;
        uint32_t timeout_ms = 1000;
    };

    struct InputFrameHolder {
        media::uvc::UvcFramePtr frame;
    };

    RK_S32 ReleaseInputFrame(void *opaque) {
        delete static_cast<InputFrameHolder *>(opaque);
        return RK_SUCCESS;
    }

    void PrintUsage(const char *program) {
        std::printf("Usage: %s [--device /dev/videoX] [--frames N] [--output FILE] [--timeout-ms N]\n",
                    program);
    }

    bool ParsePositive(const char *value, uint32_t *result) {
        char *end = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
            return false;
        }
        *result = static_cast<uint32_t>(parsed);
        return true;
    }

    bool ParseOptions(int argc, char **argv, Options *options) {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help" || argument == "-h") {
                PrintUsage(argv[0]);
                std::exit(0);
            }
            if ((argument == "--device" || argument == "--frames" || argument == "--output" ||
                 argument == "--timeout-ms") &&
                i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", argument.c_str());
                return false;
            }
            if (argument == "--device") {
                options->device = argv[++i];
            } else if (argument == "--frames") {
                if (!ParsePositive(argv[++i], &options->frames)) {
                    std::fprintf(stderr, "Invalid frame count\n");
                    return false;
                }
            } else if (argument == "--output") {
                options->output = argv[++i];
            } else if (argument == "--timeout-ms") {
                if (!ParsePositive(argv[++i], &options->timeout_ms) || options->timeout_ms > INT_MAX) {
                    std::fprintf(stderr, "Invalid timeout\n");
                    return false;
                }
            } else {
                std::fprintf(stderr, "Unknown argument: %s\n", argument.c_str());
                return false;
            }
        }
        return true;
    }

    class VdecProbe {
    public:
        explicit VdecProbe(const Options &options) : options_(options) {}
        ~VdecProbe() { Shutdown(); }

        VdecProbe(const VdecProbe &) = delete;
        VdecProbe &operator=(const VdecProbe &) = delete;

        bool Init(uint32_t width, uint32_t height) {
            width_ = width;
            height_ = height;

            RK_S32 result = RK_MPI_SYS_Init();
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_SYS_Init failed: {:#x}", result);
                return false;
            }
            mpi_initialized_ = true;

            VDEC_PIC_BUF_ATTR_S picture{};
            picture.enCodecType = RK_VIDEO_ID_MJPEG;
            picture.stPicBufAttr.u32Width = width_;
            picture.stPicBufAttr.u32Height = height_;
            picture.stPicBufAttr.enPixelFormat = RK_FMT_YUV420SP;
            picture.stPicBufAttr.enCompMode = COMPRESS_MODE_NONE;
            MB_PIC_CAL_S layout{};
            result = RK_MPI_CAL_VDEC_GetPicBufferSize(&picture, &layout);
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_CAL_VDEC_GetPicBufferSize failed: {:#x}", result);
                return false;
            }

            VDEC_CHN_ATTR_S attributes{};
            attributes.enMode = VIDEO_MODE_FRAME;
            attributes.enType = RK_VIDEO_ID_MJPEG;
            attributes.u32PicWidth = width_;
            attributes.u32PicHeight = height_;
            attributes.u32PicVirWidth = layout.u32VirWidth;
            attributes.u32PicVirHeight = layout.u32VirHeight;
            attributes.u32FrameBufSize = layout.u32MBSize;
            attributes.u32FrameBufCnt = 2;
            attributes.u32StreamBufCnt = 2;
            attributes.u32FrameBufDepth = 1;
            result = RK_MPI_VDEC_CreateChn(channel_, &attributes);
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_VDEC_CreateChn({}) failed: {:#x}", channel_, result);
                return false;
            }
            channel_created_ = true;

            VDEC_CHN_PARAM_S parameters{};
            parameters.enType = RK_VIDEO_ID_MJPEG;
            parameters.stVdecPictureParam.enPixelFormat = RK_FMT_YUV420SP;
            result = RK_MPI_VDEC_SetChnParam(channel_, &parameters);
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_VDEC_SetChnParam({}) failed: {:#x}", channel_, result);
                return false;
            }

            result = RK_MPI_VDEC_StartRecvStream(channel_);
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_VDEC_StartRecvStream({}) failed: {:#x}", channel_, result);
                return false;
            }
            receiving_ = true;
            LOG_INFO("RKMPI MJPEG VDEC probe initialized: channel={}, {}x{}, frame_buffer={} bytes",
                     channel_, width_, height_, layout.u32MBSize);
            return true;
        }

        bool Decode(const media::uvc::UvcFramePtr &input) {
            if (!receiving_ || !input || input->data.size() < 4 || input->data.size() > UINT32_MAX ||
                input->data[0] != 0xff || input->data[1] != 0xd8) {
                LOG_ERROR("Invalid MJPEG input frame for VDEC");
                return false;
            }

            auto *holder = new (std::nothrow) InputFrameHolder{input};
            if (!holder) {
                LOG_ERROR("Failed to allocate VDEC input frame holder");
                return false;
            }

            MB_EXT_CONFIG_S external{};
            external.pu8VirAddr = const_cast<RK_U8 *>(input->data.data());
            external.u64Size = input->data.size();
            external.pFreeCB = ReleaseInputFrame;
            external.pOpaque = holder;
            MB_BLK input_block = MB_INVALID_HANDLE;
            RK_S32 result = RK_MPI_SYS_CreateMB(&input_block, &external);
            if (result != RK_SUCCESS || input_block == MB_INVALID_HANDLE) {
                delete holder;
                LOG_ERROR("RK_MPI_SYS_CreateMB failed for UVC frame {}: {:#x}", input->sequence, result);
                return false;
            }

            VDEC_STREAM_S stream{};
            stream.pMbBlk = input_block;
            stream.u32Len = static_cast<RK_U32>(input->data.size());
            stream.u64PTS = input->timestamp_us;
            stream.bEndOfStream = RK_FALSE;
            stream.bEndOfFrame = RK_FALSE;
            stream.bBypassMbBlk = RK_TRUE;

            const auto frame_started = std::chrono::steady_clock::now();
            const auto send_started = frame_started;
            result = RK_MPI_VDEC_SendStream(channel_, &stream, static_cast<RK_S32>(options_.timeout_ms));
            const auto send_finished = std::chrono::steady_clock::now();
            RK_MPI_MB_ReleaseMB(input_block);
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_VDEC_SendStream failed for UVC frame {}: {:#x}", input->sequence, result);
                return false;
            }

            VIDEO_FRAME_INFO_S output{};
            const auto get_started = std::chrono::steady_clock::now();
            result = RK_MPI_VDEC_GetFrame(channel_, &output, static_cast<RK_S32>(options_.timeout_ms));
            const auto get_finished = std::chrono::steady_clock::now();
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_VDEC_GetFrame failed for UVC frame {}: {:#x}", input->sequence, result);
                return false;
            }

            bool valid = ValidateOutput(output);
            if (valid && decoded_frames_ == 0) {
                valid = WriteFirstFrame(output);
            }
            const RK_S32 release_result = RK_MPI_VDEC_ReleaseFrame(channel_, &output);
            if (release_result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_VDEC_ReleaseFrame failed: {:#x}", release_result);
                return false;
            }
            if (!valid) {
                return false;
            }

            const double send_ms =
                    std::chrono::duration<double, std::milli>(send_finished - send_started).count();
            const double get_ms =
                    std::chrono::duration<double, std::milli>(get_finished - get_started).count();
            const double total_ms =
                    std::chrono::duration<double, std::milli>(get_finished - frame_started).count();
            send_time_ms_ += send_ms;
            max_send_time_ms_ = std::max(max_send_time_ms_, send_ms);
            get_time_ms_ += get_ms;
            max_get_time_ms_ = std::max(max_get_time_ms_, get_ms);
            total_time_ms_ += total_ms;
            max_total_time_ms_ = std::max(max_total_time_ms_, total_ms);

            ++decoded_frames_;
            if (decoded_frames_ == 1) {
                statistics_started_ = get_finished;
            } else if (decoded_frames_ % 100 == 0) {
                LogStatistics(get_finished);
            }
            return true;
        }

        uint32_t DecodedFrames() const { return decoded_frames_; }

        void LogSummary() {
            if (decoded_frames_ > 0) {
                LogStatistics(std::chrono::steady_clock::now());
            }
            VDEC_CHN_STATUS_S status{};
            const RK_S32 result = RK_MPI_VDEC_QueryStatus(channel_, &status);
            if (result == RK_SUCCESS) {
                LOG_INFO("VDEC status: recv={}, decoded={}, left_stream_frames={}, left_pics={}, "
                         "format_errors={}, size_errors={}, unsupported={}, packet_errors={}, stream_too_large={}",
                         status.u32RecvStreamFrames, status.u32DecodeStreamFrames, status.u32LeftStreamFrames,
                         status.u32LeftPics, status.stVdecDecErr.s32FormatErr,
                         status.stVdecDecErr.s32PicSizeErrSet, status.stVdecDecErr.s32StreamUnsprt,
                         status.stVdecDecErr.s32PackErr, status.stVdecDecErr.s32StreamSizeOver);
            } else {
                LOG_WARN("RK_MPI_VDEC_QueryStatus failed: {:#x}", result);
            }
        }

        void Shutdown() {
            if (receiving_) {
                const RK_S32 result = RK_MPI_VDEC_StopRecvStream(channel_);
                if (result != RK_SUCCESS) {
                    LOG_WARN("RK_MPI_VDEC_StopRecvStream({}) failed: {:#x}", channel_, result);
                }
                receiving_ = false;
            }
            if (channel_created_) {
                const RK_S32 result = RK_MPI_VDEC_DestroyChn(channel_);
                if (result != RK_SUCCESS) {
                    LOG_WARN("RK_MPI_VDEC_DestroyChn({}) failed: {:#x}", channel_, result);
                }
                channel_created_ = false;
            }
            if (mpi_initialized_) {
                RK_MPI_SYS_Exit();
                mpi_initialized_ = false;
            }
        }

    private:
        bool ValidateOutput(const VIDEO_FRAME_INFO_S &output) const {
            const auto &frame = output.stVFrame;
            const bool supported_format = frame.enPixelFormat == RK_FMT_YUV420SP ||
                                          frame.enPixelFormat == RK_FMT_YUV422P;
            if (!frame.pMbBlk || !supported_format ||
                frame.enCompressMode != COMPRESS_MODE_NONE || frame.u32Width != width_ ||
                frame.u32Height != height_ || frame.u32VirWidth < width_ || frame.u32VirHeight < height_) {
                LOG_ERROR("Unexpected VDEC output: format={}, compress={}, size={}x{}, virtual={}x{}",
                          static_cast<int>(frame.enPixelFormat), static_cast<int>(frame.enCompressMode),
                          frame.u32Width, frame.u32Height, frame.u32VirWidth, frame.u32VirHeight);
                return false;
            }
            return true;
        }

        bool WriteFirstFrame(const VIDEO_FRAME_INFO_S &output) {
            const auto &frame = output.stVFrame;
            const RK_S32 cache_result = RK_MPI_SYS_MmzFlushCache(frame.pMbBlk, RK_TRUE);
            if (cache_result != RK_SUCCESS) {
                LOG_ERROR("Failed to invalidate VDEC output cache: {:#x}", cache_result);
                return false;
            }
            const auto *base = static_cast<const uint8_t *>(RK_MPI_MB_Handle2VirAddr(frame.pMbBlk));
            const size_t block_size = RK_MPI_MB_GetSize(frame.pMbBlk);
            const size_t stride = RK_MPI_CAL_COMM_GetHorStride(frame.u32VirWidth, frame.enPixelFormat);
            const size_t y_plane_size = stride * frame.u32VirHeight;
            const bool is_nv12 = frame.enPixelFormat == RK_FMT_YUV420SP;
            const size_t chroma_stride = is_nv12 ? stride : stride / 2;
            const size_t chroma_rows = is_nv12 ? frame.u32VirHeight / 2 : frame.u32VirHeight;
            const size_t chroma_planes = is_nv12 ? 1 : 2;
            const size_t required_size = y_plane_size + chroma_stride * chroma_rows * chroma_planes;
            if (!base || stride < frame.u32Width || (!is_nv12 && stride % 2 != 0) ||
                block_size < required_size) {
                LOG_ERROR("Invalid VDEC output layout: block={}, stride={}, required={}", block_size, stride,
                          required_size);
                return false;
            }

            FILE *file = std::fopen(options_.output.c_str(), "wb");
            if (!file) {
                LOG_ERROR("Failed to open VDEC output '{}': {}", options_.output, std::strerror(errno));
                return false;
            }
            bool write_ok = true;
            for (uint32_t row = 0; row < frame.u32Height && write_ok; ++row) {
                write_ok = std::fwrite(base + row * stride, 1, frame.u32Width, file) == frame.u32Width;
            }
            const auto *chroma = base + y_plane_size;
            const uint32_t visible_chroma_rows = is_nv12 ? frame.u32Height / 2 : frame.u32Height;
            const size_t visible_chroma_width = is_nv12 ? frame.u32Width : frame.u32Width / 2;
            for (size_t plane = 0; plane < chroma_planes && write_ok; ++plane) {
                const auto *plane_base = chroma + plane * chroma_stride * chroma_rows;
                for (uint32_t row = 0; row < visible_chroma_rows && write_ok; ++row) {
                    write_ok = std::fwrite(plane_base + row * chroma_stride, 1, visible_chroma_width, file) ==
                               visible_chroma_width;
                }
            }
            const bool close_ok = std::fclose(file) == 0;
            if (!write_ok || !close_ok) {
                LOG_ERROR("Failed to write complete decoded frame to '{}'", options_.output);
                return false;
            }
            const size_t pixels = static_cast<size_t>(frame.u32Width) * frame.u32Height;
            const size_t packed_size = is_nv12 ? pixels * 3 / 2 : pixels * 2;
            LOG_INFO("First VDEC output saved to '{}': format={}, size={}x{}, virtual={}x{}, stride={}, "
                     "block={} bytes, packed={} bytes",
                     options_.output, is_nv12 ? "NV12" : "YUV422P",
                     frame.u32Width, frame.u32Height, frame.u32VirWidth, frame.u32VirHeight, stride, block_size,
                     packed_size);
            return true;
        }

        void LogStatistics(std::chrono::steady_clock::time_point now) const {
            const double seconds = std::chrono::duration<double>(now - statistics_started_).count();
            LOG_INFO("MJPEG VDEC: frames={}, fps={:.2f}, send_ms(avg/max)={:.3f}/{:.3f}, "
                     "get_ms(avg/max)={:.3f}/{:.3f}, total_ms(avg/max)={:.3f}/{:.3f}",
                     decoded_frames_, seconds > 0.0 && decoded_frames_ > 1 ? (decoded_frames_ - 1) / seconds : 0.0,
                     send_time_ms_ / decoded_frames_, max_send_time_ms_, get_time_ms_ / decoded_frames_,
                     max_get_time_ms_, total_time_ms_ / decoded_frames_, max_total_time_ms_);
        }

        const Options &options_;
        const VDEC_CHN channel_ = 0;
        uint32_t width_ = 0;
        uint32_t height_ = 0;
        bool mpi_initialized_ = false;
        bool channel_created_ = false;
        bool receiving_ = false;
        uint32_t decoded_frames_ = 0;
        double send_time_ms_ = 0.0;
        double max_send_time_ms_ = 0.0;
        double get_time_ms_ = 0.0;
        double max_get_time_ms_ = 0.0;
        double total_time_ms_ = 0.0;
        double max_total_time_ms_ = 0.0;
        std::chrono::steady_clock::time_point statistics_started_;
    };

    int Run(const Options &options) {
        media::uvc::UvcConfig config;
        config.device = options.device;
        media::uvc::UvcProducer producer(config);
        VdecProbe probe(options);
        if (!probe.Init(config.width, config.height)) {
            return 1;
        }

        std::mutex mutex;
        std::condition_variable ready;
        uint32_t decoded = 0;
        bool failed = false;
        producer.SetFrameCallback([&](media::uvc::UvcFramePtr frame) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (failed || decoded >= options.frames) {
                    return;
                }
            }
            const bool decode_ok = probe.Decode(frame);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!decode_ok) {
                    failed = true;
                } else {
                    decoded = probe.DecodedFrames();
                }
            }
            if (!decode_ok || decoded >= options.frames) {
                ready.notify_one();
            }
        });

        if (producer.Init() != 0 || !producer.Start()) {
            return 1;
        }
        const auto timeout = std::chrono::seconds(std::max<uint32_t>(15, options.frames / config.fps * 3 + 10));
        bool completed = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            completed = ready.wait_for(lock, timeout, [&] { return failed || decoded >= options.frames; });
        }
        producer.Stop();
        probe.LogSummary();
        if (!completed) {
            LOG_ERROR("Timed out after decoding {} of {} requested frame(s)", decoded, options.frames);
            return 1;
        }
        if (failed) {
            LOG_ERROR("VDEC probe failed after {} decoded frame(s)", decoded);
            return 1;
        }
        LOG_INFO("VDEC probe completed: {} frame(s), output='{}'", decoded, options.output);
        return 0;
    }

} // namespace

int main(int argc, char **argv) {
    LogManager::Init();
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintUsage(argv[0]);
        LogManager::Shutdown();
        return 2;
    }
    const int result = Run(options);
    LogManager::Shutdown();
    return result;
}
