#define LOG_TAG "UVC-Hardware"

#include "uvc_hardware_pipeline.h"
#include "common/logger.h"

#include "librga/im2d.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vdec.h"

#include <chrono>
#include <climits>
#include <dlfcn.h>
#include <new>
#include <utility>

namespace media::uvc {
    namespace {
        struct InputFrameHolder {
            UvcFramePtr frame;
        };

        RK_S32 ReleaseInputFrame(void *opaque) {
            delete static_cast<InputFrameHolder *>(opaque);
            return RK_SUCCESS;
        }

        struct VdecApi {
            using CreateChnFn = decltype(&RK_MPI_VDEC_CreateChn);
            using DestroyChnFn = decltype(&RK_MPI_VDEC_DestroyChn);
            using StartRecvFn = decltype(&RK_MPI_VDEC_StartRecvStream);
            using StopRecvFn = decltype(&RK_MPI_VDEC_StopRecvStream);
            using SendStreamFn = decltype(&RK_MPI_VDEC_SendStream);
            using GetFrameFn = decltype(&RK_MPI_VDEC_GetFrame);
            using QueryStatusFn = decltype(&RK_MPI_VDEC_QueryStatus);
            using SetChnParamFn = decltype(&RK_MPI_VDEC_SetChnParam);
            using GetPicBufferSizeFn = decltype(&RK_MPI_CAL_VDEC_GetPicBufferSize);
            using SysInitFn = decltype(&RK_MPI_SYS_Init);
            using SysExitFn = decltype(&RK_MPI_SYS_Exit);
            using SysCreateMbFn = decltype(&RK_MPI_SYS_CreateMB);
            using MbReleaseFn = decltype(&RK_MPI_MB_ReleaseMB);

            bool Load() {
                if (handle) {
                    return true;
                }
                handle = dlopen("librockit_full.so", RTLD_NOW | RTLD_LOCAL);
                if (!handle) {
                    LOG_ERROR("Failed to load UVC VDEC runtime librockit_full.so: {}", dlerror());
                    return false;
                }
#define LOAD_VDEC(member, symbol) \
                member = reinterpret_cast<decltype(member)>(dlsym(handle, symbol)); \
                if (!member) { \
                    LOG_ERROR("Missing VDEC symbol {}: {}", symbol, dlerror()); \
                    dlclose(handle); \
                    handle = nullptr; \
                    return false; \
                }
                LOAD_VDEC(create_chn, "RK_MPI_VDEC_CreateChn")
                LOAD_VDEC(destroy_chn, "RK_MPI_VDEC_DestroyChn")
                LOAD_VDEC(start_recv, "RK_MPI_VDEC_StartRecvStream")
                LOAD_VDEC(stop_recv, "RK_MPI_VDEC_StopRecvStream")
                LOAD_VDEC(send_stream, "RK_MPI_VDEC_SendStream")
                LOAD_VDEC(get_frame, "RK_MPI_VDEC_GetFrame")
                LOAD_VDEC(release_frame, "RK_MPI_VDEC_ReleaseFrame")
                LOAD_VDEC(query_status, "RK_MPI_VDEC_QueryStatus")
                LOAD_VDEC(set_chn_param, "RK_MPI_VDEC_SetChnParam")
                LOAD_VDEC(get_pic_buffer_size, "RK_MPI_CAL_VDEC_GetPicBufferSize")
                LOAD_VDEC(sys_init, "RK_MPI_SYS_Init")
                LOAD_VDEC(sys_exit, "RK_MPI_SYS_Exit")
                LOAD_VDEC(sys_create_mb, "RK_MPI_SYS_CreateMB")
                LOAD_VDEC(mb_release, "RK_MPI_MB_ReleaseMB")
#undef LOAD_VDEC
                LOG_INFO("Loaded UVC VDEC runtime on demand from librockit_full.so");
                return true;
            }

            void *handle = nullptr;
            CreateChnFn create_chn = nullptr;
            DestroyChnFn destroy_chn = nullptr;
            StartRecvFn start_recv = nullptr;
            StopRecvFn stop_recv = nullptr;
            SendStreamFn send_stream = nullptr;
            GetFrameFn get_frame = nullptr;
            VdecReleaseFrameFn release_frame = nullptr;
            QueryStatusFn query_status = nullptr;
            SetChnParamFn set_chn_param = nullptr;
            GetPicBufferSizeFn get_pic_buffer_size = nullptr;
            SysInitFn sys_init = nullptr;
            SysExitFn sys_exit = nullptr;
            SysCreateMbFn sys_create_mb = nullptr;
            MbReleaseFn mb_release = nullptr;
        };

        VdecApi &GetVdecApi() {
            static VdecApi api;
            return api;
        }
    } // namespace

    VdecFrame::~VdecFrame() { Reset(); }
    VdecFrame::VdecFrame(VdecFrame &&other) noexcept { *this = std::move(other); }
    VdecFrame &VdecFrame::operator=(VdecFrame &&other) noexcept {
        if (this != &other) {
            Reset();
            channel_ = other.channel_;
            frame_ = other.frame_;
            release_ = other.release_;
            other.frame_ = {};
            other.release_ = nullptr;
        }
        return *this;
    }
    void VdecFrame::Assign(VDEC_CHN channel, VIDEO_FRAME_INFO_S frame, VdecReleaseFrameFn release) {
        Reset();
        channel_ = channel;
        frame_ = frame;
        release_ = release;
    }
    void VdecFrame::Reset() {
        if (frame_.stVFrame.pMbBlk != MB_INVALID_HANDLE) {
            const RK_S32 result = release_ ? release_(channel_, &frame_) : RK_FAILURE;
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_VDEC_ReleaseFrame failed: {:#x}", result);
            }
            frame_ = {};
            release_ = nullptr;
        }
    }

    Nv12Frame::~Nv12Frame() { Reset(); }
    Nv12Frame::Nv12Frame(Nv12Frame &&other) noexcept { *this = std::move(other); }
    Nv12Frame &Nv12Frame::operator=(Nv12Frame &&other) noexcept {
        if (this != &other) {
            Reset();
            block_ = other.block_;
            layout_ = other.layout_;
            other.block_ = MB_INVALID_HANDLE;
            other.layout_ = {};
        }
        return *this;
    }
    void Nv12Frame::Assign(MB_BLK block, const MB_PIC_CAL_S &layout) {
        Reset();
        block_ = block;
        layout_ = layout;
    }
    void Nv12Frame::Reset() {
        if (block_ != MB_INVALID_HANDLE) {
            const RK_S32 result = RK_MPI_MB_ReleaseMB(block_);
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_MB_ReleaseMB failed for NV12 frame: {:#x}", result);
            }
            block_ = MB_INVALID_HANDLE;
            layout_ = {};
        }
    }

    MjpegVdecDecoder::~MjpegVdecDecoder() { Deinit(); }
    bool MjpegVdecDecoder::Init(uint32_t width, uint32_t height, VDEC_CHN channel, uint32_t timeout_ms) {
        Deinit();
        width_ = width;
        height_ = height;
        channel_ = channel;
        timeout_ms_ = timeout_ms;
        auto &api = GetVdecApi();
        if (!api.Load()) {
            return false;
        }
        RK_S32 result = api.sys_init();
        if (result != RK_SUCCESS) {
            LOG_ERROR("UVC VDEC runtime RK_MPI_SYS_Init failed: {:#x}", result);
            return false;
        }
        runtime_initialized_ = true;

        VDEC_PIC_BUF_ATTR_S picture{};
        picture.enCodecType = RK_VIDEO_ID_MJPEG;
        picture.stPicBufAttr.u32Width = width_;
        picture.stPicBufAttr.u32Height = height_;
        picture.stPicBufAttr.enPixelFormat = RK_FMT_YUV420SP;
        picture.stPicBufAttr.enCompMode = COMPRESS_MODE_NONE;
        MB_PIC_CAL_S layout{};
        result = api.get_pic_buffer_size(&picture, &layout);
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
        result = api.create_chn(channel_, &attributes);
        if (result != RK_SUCCESS) {
            LOG_ERROR("RK_MPI_VDEC_CreateChn({}) failed: {:#x}", channel_, result);
            return false;
        }
        channel_created_ = true;

        VDEC_CHN_PARAM_S parameters{};
        parameters.enType = RK_VIDEO_ID_MJPEG;
        parameters.stVdecPictureParam.enPixelFormat = RK_FMT_YUV420SP;
        result = api.set_chn_param(channel_, &parameters);
        if (result != RK_SUCCESS) {
            LOG_ERROR("RK_MPI_VDEC_SetChnParam({}) failed: {:#x}", channel_, result);
            Deinit();
            return false;
        }
        result = api.start_recv(channel_);
        if (result != RK_SUCCESS) {
            LOG_ERROR("RK_MPI_VDEC_StartRecvStream({}) failed: {:#x}", channel_, result);
            Deinit();
            return false;
        }
        receiving_ = true;
        LOG_INFO("MJPEG VDEC initialized: channel={}, {}x{}, frame_buffer={} bytes",
                 channel_, width_, height_, layout.u32MBSize);
        return true;
    }

    void MjpegVdecDecoder::Deinit() {
        auto &api = GetVdecApi();
        if (receiving_) {
            const RK_S32 result = api.stop_recv(channel_);
            if (result != RK_SUCCESS) {
                LOG_WARN("RK_MPI_VDEC_StopRecvStream({}) failed: {:#x}", channel_, result);
            }
            receiving_ = false;
        }
        if (channel_created_) {
            const RK_S32 result = api.destroy_chn(channel_);
            if (result != RK_SUCCESS) {
                LOG_WARN("RK_MPI_VDEC_DestroyChn({}) failed: {:#x}", channel_, result);
            }
            channel_created_ = false;
        }
        if (runtime_initialized_) {
            const RK_S32 result = api.sys_exit();
            if (result != RK_SUCCESS) {
                LOG_WARN("UVC VDEC runtime RK_MPI_SYS_Exit failed: {:#x}", result);
            }
            runtime_initialized_ = false;
        }
    }

    bool MjpegVdecDecoder::Decode(const UvcFramePtr &input, VdecFrame *output, VdecTiming *timing) {
        if (!receiving_ || !input || !output || !timing || input->data.size() < 4 ||
            input->data.size() > UINT32_MAX || input->data[0] != 0xff || input->data[1] != 0xd8) {
            LOG_ERROR("Invalid MJPEG input frame for VDEC");
            return false;
        }
        output->Reset();
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
        auto &api = GetVdecApi();
        RK_S32 result = api.sys_create_mb(&input_block, &external);
        if (result != RK_SUCCESS || input_block == MB_INVALID_HANDLE) {
            delete holder;
            LOG_ERROR("RK_MPI_SYS_CreateMB failed for UVC frame {}: {:#x}", input->sequence, result);
            return false;
        }

        VDEC_STREAM_S stream{};
        stream.pMbBlk = input_block;
        stream.u32Len = static_cast<RK_U32>(input->data.size());
        stream.u64PTS = input->timestamp_us;
        stream.bEndOfFrame = RK_FALSE;
        stream.bBypassMbBlk = RK_TRUE;
        const auto send_started = std::chrono::steady_clock::now();
        result = api.send_stream(channel_, &stream, static_cast<RK_S32>(timeout_ms_));
        const auto send_finished = std::chrono::steady_clock::now();
        api.mb_release(input_block);
        timing->send_ms = std::chrono::duration<double, std::milli>(send_finished - send_started).count();
        if (result != RK_SUCCESS) {
            LOG_ERROR("RK_MPI_VDEC_SendStream failed for UVC frame {}: {:#x}", input->sequence, result);
            return false;
        }

        VIDEO_FRAME_INFO_S frame{};
        const auto get_started = std::chrono::steady_clock::now();
        result = api.get_frame(channel_, &frame, static_cast<RK_S32>(timeout_ms_));
        const auto get_finished = std::chrono::steady_clock::now();
        timing->get_ms = std::chrono::duration<double, std::milli>(get_finished - get_started).count();
        if (result != RK_SUCCESS) {
            LOG_ERROR("RK_MPI_VDEC_GetFrame failed for UVC frame {}: {:#x}", input->sequence, result);
            return false;
        }
        const auto &decoded = frame.stVFrame;
        const bool supported = decoded.enPixelFormat == RK_FMT_YUV420SP ||
                               decoded.enPixelFormat == RK_FMT_YUV422P;
        if (!decoded.pMbBlk || !supported || decoded.enCompressMode != COMPRESS_MODE_NONE ||
            decoded.u32Width != width_ || decoded.u32Height != height_ ||
            decoded.u32VirWidth < width_ || decoded.u32VirHeight < height_) {
            LOG_ERROR("Unexpected VDEC output: format={}, compress={}, size={}x{}, virtual={}x{}",
                      static_cast<int>(decoded.enPixelFormat), static_cast<int>(decoded.enCompressMode),
                      decoded.u32Width, decoded.u32Height, decoded.u32VirWidth, decoded.u32VirHeight);
            api.release_frame(channel_, &frame);
            return false;
        }
        output->Assign(channel_, frame, api.release_frame);
        return true;
    }

    void MjpegVdecDecoder::LogStatus() const {
        if (!channel_created_) {
            return;
        }
        VDEC_CHN_STATUS_S status{};
        const RK_S32 result = GetVdecApi().query_status(channel_, &status);
        if (result == RK_SUCCESS) {
            LOG_INFO("VDEC status: format_errors={}, size_errors={}, unsupported={}, packet_errors={}, "
                     "stream_too_large={}", status.stVdecDecErr.s32FormatErr,
                     status.stVdecDecErr.s32PicSizeErrSet, status.stVdecDecErr.s32StreamUnsprt,
                     status.stVdecDecErr.s32PackErr, status.stVdecDecErr.s32StreamSizeOver);
        } else {
            LOG_WARN("RK_MPI_VDEC_QueryStatus failed: {:#x}", result);
        }
    }

    RgaNv12Converter::~RgaNv12Converter() { Deinit(); }
    bool RgaNv12Converter::Init(uint32_t width, uint32_t height, uint32_t buffer_count) {
        Deinit();
        width_ = width;
        height_ = height;
        PIC_BUF_ATTR_S picture{};
        picture.u32Width = width_;
        picture.u32Height = height_;
        picture.enPixelFormat = RK_FMT_YUV420SP;
        picture.enCompMode = COMPRESS_MODE_NONE;
        RK_S32 result = RK_MPI_CAL_COMM_GetPicBufferSize(&picture, &layout_);
        if (result != RK_SUCCESS) {
            LOG_ERROR("RK_MPI_CAL_COMM_GetPicBufferSize(NV12) failed: {:#x}", result);
            return false;
        }
        MB_POOL_CONFIG_S config{};
        config.u64MBSize = layout_.u32MBSize;
        config.u32MBCnt = buffer_count;
        config.enRemapMode = MB_REMAP_MODE_CACHED;
        config.enAllocType = MB_ALLOC_TYPE_DMA;
        config.enDmaType = MB_DMA_TYPE_CMA;
        config.bPreAlloc = RK_TRUE;
        pool_ = RK_MPI_MB_CreatePool(&config);
        if (pool_ == MB_INVALID_POOLID) {
            LOG_ERROR("RK_MPI_MB_CreatePool failed for {} NV12 conversion buffer(s)", buffer_count);
            return false;
        }
        LOG_INFO("RGA NV12 pool initialized: buffers={}, virtual={}x{}, buffer={} bytes",
                 buffer_count, layout_.u32VirWidth, layout_.u32VirHeight, layout_.u32MBSize);
        return true;
    }

    void RgaNv12Converter::Deinit() {
        if (pool_ != MB_INVALID_POOLID) {
            const RK_S32 result = RK_MPI_MB_DestroyPool(pool_);
            if (result != RK_SUCCESS) {
                LOG_WARN("RK_MPI_MB_DestroyPool failed: {:#x}", result);
            }
            pool_ = MB_INVALID_POOLID;
        }
        layout_ = {};
    }

    bool RgaNv12Converter::Convert(const VdecFrame &input, Nv12Frame *output, RgaTiming *timing) {
        if (!input || !output || !timing || pool_ == MB_INVALID_POOLID) {
            return false;
        }
        output->Reset();
        const auto &source_frame = input.Info().stVFrame;
        if (source_frame.enPixelFormat != RK_FMT_YUV422P) {
            LOG_ERROR("RGA converter expected YUV422P, got format={}",
                      static_cast<int>(source_frame.enPixelFormat));
            return false;
        }
        const auto get_started = std::chrono::steady_clock::now();
        MB_BLK destination = RK_MPI_MB_GetMB(pool_, layout_.u32MBSize, RK_TRUE);
        const auto get_finished = std::chrono::steady_clock::now();
        timing->mb_get_ms = std::chrono::duration<double, std::milli>(get_finished - get_started).count();
        if (destination == MB_INVALID_HANDLE) {
            LOG_ERROR("RK_MPI_MB_GetMB failed for NV12 conversion buffer");
            return false;
        }
        const RK_S32 source_fd = RK_MPI_MB_Handle2Fd(source_frame.pMbBlk);
        const RK_S32 destination_fd = RK_MPI_MB_Handle2Fd(destination);
        bool valid = source_fd >= 0 && destination_fd >= 0;
        if (!valid) {
            LOG_ERROR("Failed to obtain RGA DMA fd: source={}, destination={}", source_fd, destination_fd);
        }
        const auto convert_started = std::chrono::steady_clock::now();
        if (valid) {
            rga_buffer_t source = wrapbuffer_fd(source_fd, source_frame.u32Width, source_frame.u32Height,
                                                RK_FORMAT_YCbCr_422_P,
                                                static_cast<int>(source_frame.u32VirWidth),
                                                static_cast<int>(source_frame.u32VirHeight));
            rga_buffer_t target = wrapbuffer_fd(destination_fd, width_, height_, RK_FORMAT_YCbCr_420_SP,
                                                static_cast<int>(layout_.u32VirWidth),
                                                static_cast<int>(layout_.u32VirHeight));
            im_rect source_rect{};
            im_rect target_rect{};
            IM_STATUS status = imcheck(source, target, source_rect, target_rect);
            if (status != IM_STATUS_NOERROR) {
                LOG_ERROR("RGA YUV422P-to-NV12 check failed: {} ({})", imStrError(status),
                          static_cast<int>(status));
                valid = false;
            } else {
                status = imcvtcolor(source, target, source.format, target.format);
                if (status != IM_STATUS_SUCCESS) {
                    LOG_ERROR("RGA YUV422P-to-NV12 conversion failed: {} ({})", imStrError(status),
                              static_cast<int>(status));
                    valid = false;
                }
            }
        }
        const auto convert_finished = std::chrono::steady_clock::now();
        timing->convert_ms =
                std::chrono::duration<double, std::milli>(convert_finished - convert_started).count();
        if (!valid) {
            RK_MPI_MB_ReleaseMB(destination);
            return false;
        }
        output->Assign(destination, layout_);
        return true;
    }

} // namespace media::uvc
