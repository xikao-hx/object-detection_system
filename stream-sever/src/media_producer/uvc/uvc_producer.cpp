#define LOG_TAG "UVCProducer"

#include "uvc_producer.h"
#include "common/logger.h"

#include <linux/videodev2.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace media::uvc {
    namespace {

        int Xioctl(int fd, unsigned long request, void *argument) {
            int result;
            do {
                result = ioctl(fd, request, argument);
            } while (result == -1 && errno == EINTR);
            return result;
        }

        uint32_t EffectiveCapabilities(const v4l2_capability &capability) {
            if ((capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) {
                return capability.device_caps;
            }
            return capability.capabilities;
        }

        bool SupportsFrameSize(int fd, uint32_t pixel_format, uint32_t width, uint32_t height) {
            v4l2_frmsizeenum size{};
            size.pixel_format = pixel_format;

            for (size.index = 0; Xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0; ++size.index) {
                if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                    if (size.discrete.width == width && size.discrete.height == height) {
                        return true;
                    }
                    continue;
                }

                if (size.type != V4L2_FRMSIZE_TYPE_CONTINUOUS && size.type != V4L2_FRMSIZE_TYPE_STEPWISE) {
                    continue;
                }
                const auto &range = size.stepwise;
                if (width < range.min_width || width > range.max_width ||
                    height < range.min_height || height > range.max_height) {
                    continue;
                }
                const bool width_aligned = range.step_width == 0 || (width - range.min_width) % range.step_width == 0;
                const bool height_aligned = range.step_height == 0 || (height - range.min_height) % range.step_height == 0;
                if (width_aligned && height_aligned) {
                    return true;
                }
            }
            return false;
        }

        double IntervalSeconds(const v4l2_fract &interval) {
            if (interval.denominator == 0) {
                return 0.0;
            }
            return static_cast<double>(interval.numerator) / interval.denominator;
        }

        bool SupportsFrameRate(int fd, const UvcConfig &config) {
            v4l2_frmivalenum interval{};
            interval.pixel_format = config.pixel_format;
            interval.width = config.width;
            interval.height = config.height;
            const double target = 1.0 / config.fps;

            for (interval.index = 0; Xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0; ++interval.index) {
                if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                    if (std::abs(IntervalSeconds(interval.discrete) - target) < 0.00001) {
                        return true;
                    }
                    continue;
                }
                if (interval.type != V4L2_FRMIVAL_TYPE_CONTINUOUS && interval.type != V4L2_FRMIVAL_TYPE_STEPWISE) {
                    continue;
                }
                const double minimum = IntervalSeconds(interval.stepwise.min);
                const double maximum = IntervalSeconds(interval.stepwise.max);
                if (target + 0.00001 >= minimum && target <= maximum + 0.00001) {
                    return true;
                }
            }
            return false;
        }

        bool ProbeDevice(const std::string &path, const UvcConfig &config, std::string *reason) {
            const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                if (reason) {
                    *reason = std::strerror(errno);
                }
                return false;
            }

            v4l2_capability capability{};
            if (Xioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
                if (reason) {
                    *reason = "VIDIOC_QUERYCAP failed";
                }
                close(fd);
                return false;
            }

            const uint32_t caps = EffectiveCapabilities(capability);
            const auto *driver = reinterpret_cast<const char *>(capability.driver);
            if (std::string(driver) != "uvcvideo") {
                if (reason) {
                    *reason = "driver is not uvcvideo";
                }
                close(fd);
                return false;
            }
            if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0U || (caps & V4L2_CAP_STREAMING) == 0U) {
                if (reason) {
                    *reason = "missing Video Capture or Streaming capability";
                }
                close(fd);
                return false;
            }

            bool has_format = false;
            v4l2_fmtdesc format{};
            format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            for (format.index = 0; Xioctl(fd, VIDIOC_ENUM_FMT, &format) == 0; ++format.index) {
                if (format.pixelformat == config.pixel_format) {
                    has_format = true;
                    break;
                }
            }

            const bool has_size = has_format && SupportsFrameSize(fd, config.pixel_format, config.width, config.height);
            const bool has_fps = has_size && SupportsFrameRate(fd, config);
            close(fd);

            if (!has_format && reason) {
                *reason = "target pixel format is not supported";
            } else if (!has_size && reason) {
                *reason = "target frame size is not supported";
            } else if (!has_fps && reason) {
                *reason = "target frame rate is not supported";
            }
            return has_format && has_size && has_fps;
        }

        std::vector<std::string> VideoDevicePaths() {
            glob_t matches{};
            std::vector<std::string> paths;
            if (glob("/dev/video*", GLOB_NOSORT, nullptr, &matches) == 0) {
                paths.reserve(matches.gl_pathc);
                for (size_t i = 0; i < matches.gl_pathc; ++i) {
                    paths.emplace_back(matches.gl_pathv[i]);
                }
            }
            globfree(&matches);
            std::sort(paths.begin(), paths.end());
            return paths;
        }

    } // namespace

    std::string FindUvcDevice(const UvcConfig &config) {
        if (!config.device.empty()) {
            std::string reason;
            if (ProbeDevice(config.device, config, &reason)) {
                return config.device;
            }
            LOG_ERROR("Configured UVC device '{}' is unsuitable: {}", config.device, reason);
            return {};
        }

        for (const auto &path : VideoDevicePaths()) {
            std::string reason;
            if (ProbeDevice(path, config, &reason)) {
                return path;
            }
            LOG_DEBUG("Skipping video device '{}': {}", path, reason);
        }
        return {};
    }

    struct UvcProducer::Impl {
        struct MappedBuffer {
            void *address = MAP_FAILED;
            size_t length = 0;
        };

        explicit Impl(UvcConfig producer_config) : config(std::move(producer_config)) {}

        void CleanupBuffers() {
            for (auto &buffer : buffers) {
                if (buffer.address != MAP_FAILED) {
                    munmap(buffer.address, buffer.length);
                    buffer.address = MAP_FAILED;
                }
            }
            buffers.clear();
        }

        void CaptureLoop() {
            while (running.load()) {
                pollfd descriptor{};
                descriptor.fd = fd;
                descriptor.events = POLLIN;
                const int poll_result = poll(&descriptor, 1, config.poll_timeout_ms);
                if (poll_result == 0) {
                    LOG_WARN("Timed out waiting for a UVC frame from '{}'", device_path);
                    continue;
                }
                if (poll_result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (running.load()) {
                        LOG_ERROR("poll failed for '{}': {}", device_path, std::strerror(errno));
                    }
                    break;
                }
                if ((descriptor.revents & POLLIN) == 0) {
                    if (running.load()) {
                        LOG_ERROR("Unexpected poll events for '{}': 0x{:x}", device_path, descriptor.revents);
                    }
                    break;
                }

                v4l2_buffer buffer{};
                buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buffer.memory = V4L2_MEMORY_MMAP;
                if (Xioctl(fd, VIDIOC_DQBUF, &buffer) != 0) {
                    if (errno == EAGAIN || errno == EINTR) {
                        continue;
                    }
                    if (running.load()) {
                        LOG_ERROR("VIDIOC_DQBUF failed for '{}': {}", device_path, std::strerror(errno));
                    }
                    break;
                }

                std::shared_ptr<UvcFrame> frame;
                if (buffer.index >= buffers.size() || buffer.bytesused > buffers[buffer.index].length) {
                    LOG_ERROR("UVC driver returned an invalid buffer (index={}, bytes={})", buffer.index,
                              buffer.bytesused);
                    running.store(false);
                } else if (buffer.bytesused > 0) {
                    frame = std::make_shared<UvcFrame>();
                    const auto *begin = static_cast<const uint8_t *>(buffers[buffer.index].address);
                    frame->data.assign(begin, begin + buffer.bytesused);
                    frame->width = config.width;
                    frame->height = config.height;
                    frame->sequence = buffer.sequence;
                    frame->timestamp_us = static_cast<uint64_t>(buffer.timestamp.tv_sec) * 1000000ULL +
                                          static_cast<uint64_t>(buffer.timestamp.tv_usec);

                }

                if (Xioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
                    if (running.load()) {
                        LOG_ERROR("VIDIOC_QBUF failed for '{}': {}", device_path, std::strerror(errno));
                    }
                    break;
                }

                if (frame) {
                    try {
                        UvcFrameCallback frame_callback;
                        {
                            std::lock_guard<std::mutex> lock(callback_mutex);
                            frame_callback = callback;
                        }
                        if (frame_callback) {
                            frame_callback(std::move(frame));
                        }
                    } catch (const std::exception &error) {
                        LOG_ERROR("UVC frame callback threw an exception: {}", error.what());
                    } catch (...) {
                        LOG_ERROR("UVC frame callback threw an unknown exception");
                    }
                }
            }
            running.store(false);
        }

        UvcConfig config;
        std::string device_path;
        int fd = -1;
        std::vector<MappedBuffer> buffers;
        UvcFrameCallback callback;
        std::mutex callback_mutex;
        std::thread capture_thread;
        std::atomic<bool> initialized{false};
        std::atomic<bool> running{false};
    };

    UvcProducer::UvcProducer(UvcConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

    UvcProducer::~UvcProducer() { Deinit(); }

    bool UvcProducer::SetFrameCallback(UvcFrameCallback callback) {
        if (impl_->running.load()) {
            LOG_WARN("Cannot change UVC frame callback while producer is running");
            return false;
        }
        std::lock_guard<std::mutex> lock(impl_->callback_mutex);
        impl_->callback = std::move(callback);
        return true;
    }

    int UvcProducer::Init() {
        if (impl_->initialized.load()) {
            return 0;
        }
        const auto &config = impl_->config;
        if (config.width == 0 || config.height == 0 || config.fps == 0 || config.buffer_count < 2 ||
            config.poll_timeout_ms <= 0) {
            LOG_ERROR("Invalid UVC configuration");
            return -1;
        }

        impl_->device_path = FindUvcDevice(config);
        if (impl_->device_path.empty()) {
            LOG_ERROR("No UVC device supports MJPEG {}x{} at {} fps", config.width, config.height, config.fps);
            return -1;
        }
        LOG_INFO("Selected UVC device '{}'", impl_->device_path);

        impl_->fd = open(impl_->device_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (impl_->fd < 0) {
            LOG_ERROR("Failed to open '{}': {}", impl_->device_path, std::strerror(errno));
            return -1;
        }

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = config.width;
        format.fmt.pix.height = config.height;
        format.fmt.pix.pixelformat = config.pixel_format;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        if (Xioctl(impl_->fd, VIDIOC_S_FMT, &format) != 0 || format.fmt.pix.width != config.width ||
            format.fmt.pix.height != config.height || format.fmt.pix.pixelformat != config.pixel_format) {
            LOG_ERROR("Failed to set UVC format to {}x{} MJPEG: {}", config.width, config.height,
                      std::strerror(errno));
            Deinit();
            return -1;
        }

        v4l2_streamparm parameters{};
        parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parameters.parm.capture.timeperframe.numerator = 1;
        parameters.parm.capture.timeperframe.denominator = config.fps;
        if (Xioctl(impl_->fd, VIDIOC_S_PARM, &parameters) != 0) {
            LOG_ERROR("Failed to set UVC frame rate to {} fps: {}", config.fps, std::strerror(errno));
            Deinit();
            return -1;
        }
        const auto &actual_interval = parameters.parm.capture.timeperframe;
        if (actual_interval.numerator == 0 || actual_interval.denominator != config.fps * actual_interval.numerator) {
            LOG_ERROR("UVC device did not accept {} fps (actual {}/{})", config.fps, actual_interval.denominator,
                      actual_interval.numerator);
            Deinit();
            return -1;
        }

        v4l2_requestbuffers request{};
        request.count = config.buffer_count;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (Xioctl(impl_->fd, VIDIOC_REQBUFS, &request) != 0 || request.count < 2) {
            LOG_ERROR("Failed to request at least two UVC mmap buffers: {}", std::strerror(errno));
            Deinit();
            return -1;
        }

        impl_->buffers.resize(request.count);
        for (uint32_t index = 0; index < request.count; ++index) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (Xioctl(impl_->fd, VIDIOC_QUERYBUF, &buffer) != 0) {
                LOG_ERROR("VIDIOC_QUERYBUF failed for buffer {}: {}", index, std::strerror(errno));
                Deinit();
                return -1;
            }
            auto &mapped = impl_->buffers[index];
            mapped.length = buffer.length;
            mapped.address = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, impl_->fd,
                                  buffer.m.offset);
            if (mapped.address == MAP_FAILED) {
                LOG_ERROR("mmap failed for UVC buffer {}: {}", index, std::strerror(errno));
                Deinit();
                return -1;
            }
        }

        impl_->initialized.store(true);
        LOG_INFO("UVC producer initialized with {} mmap buffer(s)", impl_->buffers.size());
        return 0;
    }

    int UvcProducer::Deinit() {
        Stop();
        impl_->CleanupBuffers();
        if (impl_->fd >= 0) {
            close(impl_->fd);
            impl_->fd = -1;
        }
        impl_->initialized.store(false);
        impl_->device_path.clear();
        return 0;
    }

    bool UvcProducer::Start() {
        if (impl_->capture_thread.joinable()) {
            Stop();
        }
        if (!impl_->initialized.load() || impl_->running.load()) {
            return impl_->running.load();
        }
        {
            std::lock_guard<std::mutex> lock(impl_->callback_mutex);
            if (!impl_->callback) {
                LOG_ERROR("Cannot start UVC producer without a frame callback");
                return false;
            }
        }

        for (uint32_t index = 0; index < impl_->buffers.size(); ++index) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (Xioctl(impl_->fd, VIDIOC_QBUF, &buffer) != 0) {
                LOG_ERROR("VIDIOC_QBUF failed for buffer {}: {}", index, std::strerror(errno));
                return false;
            }
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (Xioctl(impl_->fd, VIDIOC_STREAMON, &type) != 0) {
            LOG_ERROR("VIDIOC_STREAMON failed for '{}': {}", impl_->device_path, std::strerror(errno));
            return false;
        }

        impl_->running.store(true);
        try {
            impl_->capture_thread = std::thread(&Impl::CaptureLoop, impl_.get());
        } catch (const std::exception &error) {
            impl_->running.store(false);
            Xioctl(impl_->fd, VIDIOC_STREAMOFF, &type);
            LOG_ERROR("Failed to create UVC capture thread: {}", error.what());
            return false;
        }
        LOG_INFO("UVC producer started");
        return true;
    }

    void UvcProducer::Stop() {
        const bool had_thread = impl_->capture_thread.joinable();
        impl_->running.store(false);
        if (had_thread && impl_->fd >= 0) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (Xioctl(impl_->fd, VIDIOC_STREAMOFF, &type) != 0 && errno != EINVAL) {
                LOG_WARN("VIDIOC_STREAMOFF failed for '{}': {}", impl_->device_path, std::strerror(errno));
            }
        }
        if (had_thread) {
            impl_->capture_thread.join();
            LOG_INFO("UVC producer stopped");
        }
    }

    bool UvcProducer::IsInitialized() const { return impl_->initialized.load(); }

    bool UvcProducer::IsRunning() const { return impl_->running.load(); }

    const UvcConfig &UvcProducer::GetConfig() const { return impl_->config; }

    const std::string &UvcProducer::GetDevicePath() const { return impl_->device_path; }

} // namespace media::uvc
