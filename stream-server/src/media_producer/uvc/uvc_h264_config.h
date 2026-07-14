#pragma once

#include "uvc_config.h"

namespace media {

    struct UvcH264Config {
        uvc::UvcConfig capture;
        int bitrate_kbps = 4 * 1024;
        int gop = 30;
        int venc_channel = 0;
        int input_buffer_count = 4;
    };

} // namespace media
