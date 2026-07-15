#pragma once

#include "rk_mpi_venc.h"

#include <cstring>

namespace media {

    inline int InitH264Venc(int channel, int width, int height, int bitrate_kbps, int framerate, int gop,
                            int virtual_width = 0, int virtual_height = 0) {
        if (bitrate_kbps <= 0) {
            bitrate_kbps = 10 * 1024;
        }
        if (framerate <= 0) {
            framerate = 30;
        }
        if (gop <= 0) {
            gop = framerate;
        }
        if (virtual_width <= 0) {
            virtual_width = width;
        }
        if (virtual_height <= 0) {
            virtual_height = height;
        }

        VENC_CHN_ATTR_S attributes{};
        attributes.stVencAttr.enType = RK_VIDEO_ID_AVC;
        attributes.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
        attributes.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
        attributes.stVencAttr.u32PicWidth = width;
        attributes.stVencAttr.u32PicHeight = height;
        attributes.stVencAttr.u32VirWidth = virtual_width;
        attributes.stVencAttr.u32VirHeight = virtual_height;
        attributes.stVencAttr.u32StreamBufCnt = 2;
        attributes.stVencAttr.u32BufSize = virtual_width * virtual_height / 2;

        attributes.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        attributes.stRcAttr.stH264Cbr.u32Gop = gop;
        attributes.stRcAttr.stH264Cbr.u32BitRate = bitrate_kbps;
        attributes.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
        attributes.stRcAttr.stH264Cbr.fr32DstFrameRateNum = framerate;
        attributes.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
        attributes.stRcAttr.stH264Cbr.u32SrcFrameRateNum = framerate;

        RK_S32 result = RK_MPI_VENC_CreateChn(channel, &attributes);
        if (result != RK_SUCCESS) {
            return result;
        }

        VENC_RECV_PIC_PARAM_S receive{};
        receive.s32RecvPicNum = -1;
        result = RK_MPI_VENC_StartRecvFrame(channel, &receive);
        if (result != RK_SUCCESS) {
            RK_MPI_VENC_DestroyChn(channel);
        }
        return result;
    }

} // namespace media
