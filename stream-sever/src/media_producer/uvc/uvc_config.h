#pragma once

#include <linux/videodev2.h>

#include <cstdint>
#include <string>

namespace media::uvc {

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
