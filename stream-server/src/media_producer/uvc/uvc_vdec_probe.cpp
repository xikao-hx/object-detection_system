#define LOG_TAG "UVCVdecProbe"

#include "uvc_hardware_pipeline.h"
#include "uvc_producer.h"
#include "common/logger.h"

#include "rk_mpi_cal.h"
#include "rk_mpi_sys.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

    struct Options {
        std::string device;
        std::string output = "/tmp/uvc_vdec_first.yuv";
        uint32_t frames = 300;
        uint32_t timeout_ms = 1000;
        bool convert_nv12 = false;
    };

    void PrintUsage(const char *program) {
        std::printf("Usage: %s [--device /dev/videoX] [--frames N] [--output FILE] [--timeout-ms N] "
                    "[--convert-nv12]\n", program);
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
                 argument == "--timeout-ms") && i + 1 >= argc) {
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
            } else if (argument == "--convert-nv12") {
                options->convert_nv12 = true;
            } else {
                std::fprintf(stderr, "Unknown argument: %s\n", argument.c_str());
                return false;
            }
        }
        return true;
    }

    bool WritePlane(FILE *file, const uint8_t *base, size_t stride, uint32_t rows, size_t width) {
        for (uint32_t row = 0; row < rows; ++row) {
            if (std::fwrite(base + row * stride, 1, width, file) != width) {
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
            const RK_S32 result = RK_MPI_SYS_Init();
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_SYS_Init failed: {:#x}", result);
                return false;
            }
            mpi_initialized_ = true;
            if (!decoder_.Init(width_, height_, 0, options_.timeout_ms)) {
                return false;
            }
            if (options_.convert_nv12 && !converter_.Init(width_, height_, 1)) {
                return false;
            }
            return true;
        }

        bool Decode(const media::uvc::UvcFramePtr &input) {
            const auto started = std::chrono::steady_clock::now();
            media::uvc::VdecFrame decoded;
            media::uvc::VdecTiming vdec_timing;
            if (!decoder_.Decode(input, &decoded, &vdec_timing)) {
                return false;
            }

            media::uvc::RgaTiming rga_timing;
            bool valid = true;
            if (options_.convert_nv12) {
                media::uvc::Nv12Frame converted;
                valid = converter_.Convert(decoded, &converted, &rga_timing);
                if (valid && decoded_frames_ == 0) {
                    valid = WriteNv12Frame(converted);
                }
            } else if (decoded_frames_ == 0) {
                valid = WriteDecodedFrame(decoded);
            }
            const auto finished = std::chrono::steady_clock::now();
            if (!valid) {
                return false;
            }

            const double total_ms = std::chrono::duration<double, std::milli>(finished - started).count();
            Accumulate(vdec_timing.send_ms, &send_time_ms_, &max_send_time_ms_);
            Accumulate(vdec_timing.get_ms, &get_time_ms_, &max_get_time_ms_);
            Accumulate(rga_timing.convert_ms, &convert_time_ms_, &max_convert_time_ms_);
            Accumulate(total_ms, &total_time_ms_, &max_total_time_ms_);
            ++decoded_frames_;
            if (decoded_frames_ == 1) {
                statistics_started_ = finished;
            } else if (decoded_frames_ % 100 == 0) {
                LogStatistics(finished);
            }
            return true;
        }

        uint32_t DecodedFrames() const { return decoded_frames_; }

        void LogSummary() {
            if (decoded_frames_ > 0) {
                LogStatistics(std::chrono::steady_clock::now());
            }
            decoder_.LogStatus();
        }

        void Shutdown() {
            converter_.Deinit();
            decoder_.Deinit();
            if (mpi_initialized_) {
                RK_MPI_SYS_Exit();
                mpi_initialized_ = false;
            }
        }

    private:
        static void Accumulate(double value, double *sum, double *maximum) {
            *sum += value;
            *maximum = std::max(*maximum, value);
        }

        bool PrepareBlock(MB_BLK block, size_t stride, size_t required_size, uint32_t visible_width) const {
            const RK_S32 result = RK_MPI_SYS_MmzFlushCache(block, RK_TRUE);
            if (result != RK_SUCCESS) {
                LOG_ERROR("Failed to invalidate output cache: {:#x}", result);
                return false;
            }
            if (!RK_MPI_MB_Handle2VirAddr(block) || stride < visible_width ||
                RK_MPI_MB_GetSize(block) < required_size) {
                LOG_ERROR("Invalid output layout: block={}, stride={}, required={}",
                          RK_MPI_MB_GetSize(block), stride, required_size);
                return false;
            }
            return true;
        }

        bool WriteNv12Frame(const media::uvc::Nv12Frame &output) const {
            const auto &layout = output.Layout();
            const size_t stride = RK_MPI_CAL_COMM_GetHorStride(layout.u32VirWidth, RK_FMT_YUV420SP);
            const size_t y_plane_size = stride * layout.u32VirHeight;
            const size_t required_size = y_plane_size + stride * layout.u32VirHeight / 2;
            if (!PrepareBlock(output.Block(), stride, required_size, width_)) {
                return false;
            }
            const auto *base = static_cast<const uint8_t *>(RK_MPI_MB_Handle2VirAddr(output.Block()));
            FILE *file = std::fopen(options_.output.c_str(), "wb");
            if (!file) {
                LOG_ERROR("Failed to open RGA NV12 output '{}': {}", options_.output, std::strerror(errno));
                return false;
            }
            const bool write_ok = WritePlane(file, base, stride, height_, width_) &&
                                  WritePlane(file, base + y_plane_size, stride, height_ / 2, width_);
            const bool close_ok = std::fclose(file) == 0;
            if (!write_ok || !close_ok) {
                LOG_ERROR("Failed to write complete RGA NV12 frame to '{}'", options_.output);
                return false;
            }
            LOG_INFO("First RGA output saved to '{}': format=NV12, size={}x{}, virtual={}x{}, stride={}, "
                     "block={} bytes, packed={} bytes",
                     options_.output, width_, height_, layout.u32VirWidth, layout.u32VirHeight, stride,
                     RK_MPI_MB_GetSize(output.Block()), static_cast<size_t>(width_) * height_ * 3 / 2);
            return true;
        }

        bool WriteDecodedFrame(const media::uvc::VdecFrame &output) const {
            const auto &frame = output.Info().stVFrame;
            const size_t stride = RK_MPI_CAL_COMM_GetHorStride(frame.u32VirWidth, frame.enPixelFormat);
            const size_t y_plane_size = stride * frame.u32VirHeight;
            const bool is_nv12 = frame.enPixelFormat == RK_FMT_YUV420SP;
            const size_t chroma_stride = is_nv12 ? stride : stride / 2;
            const uint32_t chroma_rows = is_nv12 ? frame.u32VirHeight / 2 : frame.u32VirHeight;
            const size_t chroma_planes = is_nv12 ? 1 : 2;
            const size_t required_size = y_plane_size + chroma_stride * chroma_rows * chroma_planes;
            if ((!is_nv12 && stride % 2 != 0) ||
                !PrepareBlock(frame.pMbBlk, stride, required_size, frame.u32Width)) {
                return false;
            }
            const auto *base = static_cast<const uint8_t *>(RK_MPI_MB_Handle2VirAddr(frame.pMbBlk));
            FILE *file = std::fopen(options_.output.c_str(), "wb");
            if (!file) {
                LOG_ERROR("Failed to open VDEC output '{}': {}", options_.output, std::strerror(errno));
                return false;
            }
            bool write_ok = WritePlane(file, base, stride, frame.u32Height, frame.u32Width);
            const auto *chroma = base + y_plane_size;
            const uint32_t visible_rows = is_nv12 ? frame.u32Height / 2 : frame.u32Height;
            const size_t visible_width = is_nv12 ? frame.u32Width : frame.u32Width / 2;
            for (size_t plane = 0; plane < chroma_planes && write_ok; ++plane) {
                write_ok = WritePlane(file, chroma + plane * chroma_stride * chroma_rows,
                                      chroma_stride, visible_rows, visible_width);
            }
            const bool close_ok = std::fclose(file) == 0;
            if (!write_ok || !close_ok) {
                LOG_ERROR("Failed to write complete decoded frame to '{}'", options_.output);
                return false;
            }
            const size_t packed_size = static_cast<size_t>(frame.u32Width) * frame.u32Height *
                                       (is_nv12 ? 3 : 4) / 2;
            LOG_INFO("First VDEC output saved to '{}': format={}, size={}x{}, virtual={}x{}, stride={}, "
                     "block={} bytes, packed={} bytes",
                     options_.output, is_nv12 ? "NV12" : "YUV422P", frame.u32Width, frame.u32Height,
                     frame.u32VirWidth, frame.u32VirHeight, stride, RK_MPI_MB_GetSize(frame.pMbBlk), packed_size);
            return true;
        }

        void LogStatistics(std::chrono::steady_clock::time_point now) const {
            const double seconds = std::chrono::duration<double>(now - statistics_started_).count();
            LOG_INFO("MJPEG VDEC{}: frames={}, fps={:.2f}, send_ms(avg/max)={:.3f}/{:.3f}, "
                     "get_ms(avg/max)={:.3f}/{:.3f}, rga_ms(avg/max)={:.3f}/{:.3f}, "
                     "pipeline_ms(avg/max)={:.3f}/{:.3f}",
                     options_.convert_nv12 ? "+RGA" : "", decoded_frames_,
                     seconds > 0.0 && decoded_frames_ > 1 ? (decoded_frames_ - 1) / seconds : 0.0,
                     send_time_ms_ / decoded_frames_, max_send_time_ms_, get_time_ms_ / decoded_frames_,
                     max_get_time_ms_, convert_time_ms_ / decoded_frames_, max_convert_time_ms_,
                     total_time_ms_ / decoded_frames_, max_total_time_ms_);
        }

        const Options &options_;
        uint32_t width_ = 0;
        uint32_t height_ = 0;
        bool mpi_initialized_ = false;
        media::uvc::MjpegVdecDecoder decoder_;
        media::uvc::RgaNv12Converter converter_;
        uint32_t decoded_frames_ = 0;
        double send_time_ms_ = 0.0;
        double max_send_time_ms_ = 0.0;
        double get_time_ms_ = 0.0;
        double max_get_time_ms_ = 0.0;
        double convert_time_ms_ = 0.0;
        double max_convert_time_ms_ = 0.0;
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
