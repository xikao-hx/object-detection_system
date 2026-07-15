#pragma once

#include <linux/videodev2.h>

#include <cstdint>
#include <string>

namespace media::uvc {

    enum class Resolution {
        R_3840X1080,
        R_2560X720,
        R_1280X480,
    };

    struct ResolutionConfig {
        uint32_t width;
        uint32_t height;

        static constexpr ResolutionConfig FromPreset(Resolution preset) {
            switch (preset) {
                case Resolution::R_3840X1080:
                    return {3840, 1080};
                case Resolution::R_2560X720:
                    return {2560, 720};
                case Resolution::R_1280X480:
                default:
                    return {1280, 480};
            }
        }
    };

    struct UvcConfig {
        // 为空时自动发现满足下列规格的 uvcvideo capture 节点。
        std::string device;
        uint32_t width = 1280;
        uint32_t height = 480;
        uint32_t fps = 30;
        uint32_t pixel_format = V4L2_PIX_FMT_MJPEG;
        uint32_t buffer_count = 4;
        int poll_timeout_ms = 1000;
    };

} // namespace media::uvc
