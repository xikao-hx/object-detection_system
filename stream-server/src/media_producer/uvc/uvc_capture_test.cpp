#define LOG_TAG "UVCCaptureTest"

#include "uvc_producer.h"
#include "common/logger.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

    struct Options {
        std::string device;
        std::string output = "/tmp/uvc_first.jpg";
        uint32_t frames = 100;
    };

    void PrintUsage(const char *program) {
        std::printf("Usage: %s [--device /dev/videoX] [--frames N] [--output FILE]\n", program);
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
            if ((argument == "--device" || argument == "--frames" || argument == "--output") && i + 1 >= argc) {
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
            } else {
                std::fprintf(stderr, "Unknown argument: %s\n", argument.c_str());
                return false;
            }
        }
        return true;
    }

    bool WriteJpeg(const std::string &path, const media::uvc::UvcFrame &frame) {
        if (frame.data.size() < 4 || frame.data[0] != 0xff || frame.data[1] != 0xd8) {
            LOG_ERROR("First captured buffer is not a JPEG frame");
            return false;
        }
        FILE *file = std::fopen(path.c_str(), "wb");
        if (!file) {
            LOG_ERROR("Failed to open output '{}': {}", path, std::strerror(errno));
            return false;
        }
        const size_t written = std::fwrite(frame.data.data(), 1, frame.data.size(), file);
        const bool close_ok = std::fclose(file) == 0;
        if (written != frame.data.size() || !close_ok) {
            LOG_ERROR("Failed to write complete JPEG to '{}'", path);
            return false;
        }
        return true;
    }

    int Run(const Options &options) {
        media::uvc::UvcConfig config;
        config.device = options.device;
        media::uvc::UvcProducer producer(config);

        std::mutex mutex;
        std::condition_variable ready;
        uint32_t captured = 0;
        media::uvc::UvcFramePtr first_frame;
        std::chrono::steady_clock::time_point first_frame_at;
        std::chrono::steady_clock::time_point last_frame_at;
        producer.SetFrameCallback([&](media::uvc::UvcFramePtr frame) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!first_frame) {
                first_frame = frame;
                first_frame_at = std::chrono::steady_clock::now();
            }
            last_frame_at = std::chrono::steady_clock::now();
            ++captured;
            if (captured >= options.frames) {
                ready.notify_one();
            }
        });

        if (producer.Init() != 0 || !producer.Start()) {
            return 1;
        }

        const auto timeout = std::chrono::seconds(std::max<uint32_t>(10, options.frames / config.fps * 3 + 5));
        bool completed = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            completed = ready.wait_for(lock, timeout, [&] { return captured >= options.frames; });
        }
        producer.Stop();
        if (!completed) {
            LOG_ERROR("Timed out after capturing {} of {} requested frame(s)", captured, options.frames);
            return 1;
        }

        if (!first_frame || !WriteJpeg(options.output, *first_frame)) {
            return 1;
        }
        const double elapsed_seconds = std::chrono::duration<double>(last_frame_at - first_frame_at).count();
        const double measured_fps = elapsed_seconds > 0.0 && captured > 1 ? (captured - 1) / elapsed_seconds : 0.0;
        LOG_INFO("Captured {} frame(s) at {:.2f} fps; first frame saved to '{}' ({} bytes, {}x{})", captured,
                 measured_fps, options.output, first_frame->data.size(), first_frame->width, first_frame->height);
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
