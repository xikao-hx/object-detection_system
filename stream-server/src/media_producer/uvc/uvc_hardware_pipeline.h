#pragma once

#include "uvc_producer.h"

#include "rk_comm_video.h"
#include "rk_comm_vdec.h"
#include "rk_mpi_mb.h"

#include <cstdint>

namespace media::uvc {

    using VdecReleaseFrameFn = RK_S32 (*)(VDEC_CHN, const VIDEO_FRAME_INFO_S *);

    struct VdecTiming {
        double send_ms = 0.0;
        double get_ms = 0.0;
    };

    struct RgaTiming {
        double mb_get_ms = 0.0;
        double convert_ms = 0.0;
    };

    class VdecFrame {
    public:
        VdecFrame() = default;
        ~VdecFrame();
        VdecFrame(VdecFrame &&other) noexcept;
        VdecFrame &operator=(VdecFrame &&other) noexcept;
        VdecFrame(const VdecFrame &) = delete;
        VdecFrame &operator=(const VdecFrame &) = delete;

        const VIDEO_FRAME_INFO_S &Info() const { return frame_; }
        explicit operator bool() const { return frame_.stVFrame.pMbBlk != MB_INVALID_HANDLE; }
        void Reset();

    private:
        friend class MjpegVdecDecoder;
        void Assign(VDEC_CHN channel, VIDEO_FRAME_INFO_S frame, VdecReleaseFrameFn release);

        VDEC_CHN channel_ = 0;
        VIDEO_FRAME_INFO_S frame_{};
        VdecReleaseFrameFn release_ = nullptr;
    };

    class Nv12Frame {
    public:
        Nv12Frame() = default;
        ~Nv12Frame();
        Nv12Frame(Nv12Frame &&other) noexcept;
        Nv12Frame &operator=(Nv12Frame &&other) noexcept;
        Nv12Frame(const Nv12Frame &) = delete;
        Nv12Frame &operator=(const Nv12Frame &) = delete;

        MB_BLK Block() const { return block_; }
        const MB_PIC_CAL_S &Layout() const { return layout_; }
        explicit operator bool() const { return block_ != MB_INVALID_HANDLE; }
        void Reset();

    private:
        friend class RgaNv12Converter;
        void Assign(MB_BLK block, const MB_PIC_CAL_S &layout);

        MB_BLK block_ = MB_INVALID_HANDLE;
        MB_PIC_CAL_S layout_{};
    };

    class MjpegVdecDecoder {
    public:
        ~MjpegVdecDecoder();
        bool Init(uint32_t width, uint32_t height, VDEC_CHN channel = 0, uint32_t timeout_ms = 1000);
        void Deinit();
        bool Decode(const UvcFramePtr &input, VdecFrame *output, VdecTiming *timing);
        void LogStatus() const;

    private:
        uint32_t width_ = 0;
        uint32_t height_ = 0;
        uint32_t timeout_ms_ = 1000;
        VDEC_CHN channel_ = 0;
        bool runtime_initialized_ = false;
        bool channel_created_ = false;
        bool receiving_ = false;
    };

    class RgaNv12Converter {
    public:
        ~RgaNv12Converter();
        bool Init(uint32_t width, uint32_t height, uint32_t buffer_count);
        void Deinit();
        bool Convert(const VdecFrame &input, Nv12Frame *output, RgaTiming *timing);
        const MB_PIC_CAL_S &Layout() const { return layout_; }

    private:
        uint32_t width_ = 0;
        uint32_t height_ = 0;
        MB_POOL pool_ = MB_INVALID_POOLID;
        MB_PIC_CAL_S layout_{};
    };

} // namespace media::uvc
