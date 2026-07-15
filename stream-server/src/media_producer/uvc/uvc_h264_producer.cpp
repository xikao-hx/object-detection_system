#define LOG_TAG "UVC-H264"

#include "uvc_h264_producer.h"
#include "../encoded_stream_dispatcher.h"
#include "../rk_venc_config.h"
#include "common/logger.h"
#include "uvc_hardware_pipeline.h"
#include "uvc_producer.h"

#include "rk_mpi_cal.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace media {
    struct UvcH264Producer::Impl {
        struct PendingFrame {
            uvc::UvcFramePtr frame;
            std::chrono::steady_clock::time_point enqueued_at;
        };

        struct MailboxStatistics {
            uint64_t dropped_frames = 0;
            uint64_t dequeued_frames = 0;
            size_t current_depth = 0;
            size_t max_depth = 0;
            double average_wait_ms = 0.0;
            double max_wait_ms = 0.0;
        };

        explicit Impl(const UvcH264Config &producer_config) : config(producer_config), capture(config.capture) {
            capture.SetFrameCallback([this](uvc::UvcFramePtr frame) {
                if (accept_frames.load() && frame) {
                    QueueFrame(std::move(frame));
                }
            });
        }

        int InitMpi() {
            RK_S32 result = RK_MPI_SYS_Init();
            if (result != RK_SUCCESS) {
                LOG_ERROR("RK_MPI_SYS_Init failed: {:#x}", result);
                return -1;
            }
            mpi_initialized = true;

            if (!hardware_decoder.Init(config.capture.width, config.capture.height) ||
                !nv12_converter.Init(config.capture.width, config.capture.height, config.input_buffer_count)) {
                LOG_ERROR("Failed to initialize MJPEG VDEC/RGA pipeline");
                return -1;
            }
            buffer_layout = nv12_converter.Layout();

            result = InitH264Venc(config.venc_channel, config.capture.width, config.capture.height,
                                  config.bitrate_kbps, config.capture.fps, config.gop);
            if (result != RK_SUCCESS) {
                LOG_ERROR("Failed to initialize H.264 VENC channel {}: {:#x}", config.venc_channel, result);
                return -1;
            }
            venc_initialized = true;
            LOG_INFO("RKMPI VENC initialized: channel={}, {}x{}, bitrate={} kbps, input={} bytes",
                     config.venc_channel, config.capture.width, config.capture.height, config.bitrate_kbps,
                     buffer_layout.u32MBSize);
            return 0;
        }

        void Cleanup() {
            accept_frames.store(false);
            capture.Deinit();
            StopProcessing();
            dispatcher.Stop();

            if (venc_initialized) {
                RK_MPI_VENC_StopRecvFrame(config.venc_channel);
                RK_MPI_VENC_DestroyChn(config.venc_channel);
                venc_initialized = false;
            }
            nv12_converter.Deinit();
            hardware_decoder.Deinit();
            if (mpi_initialized) {
                RK_MPI_SYS_Exit();
                mpi_initialized = false;
            }
        }

        void ResetStatistics() {
            frames_sent = 0;
            decode_errors = 0;
            send_errors = 0;
            decode_time_ms = 0.0;
            max_decode_time_ms = 0.0;
            rga_time_ms = 0.0;
            max_rga_time_ms = 0.0;
            mb_get_time_ms = 0.0;
            max_mb_get_time_ms = 0.0;
            venc_send_time_ms = 0.0;
            max_venc_send_time_ms = 0.0;
            total_time_ms = 0.0;
            max_total_time_ms = 0.0;
            statistics_started = {};
        }

        bool StartProcessing() {
            if (processing_thread.joinable()) {
                StopProcessing();
            }
            {
                std::lock_guard<std::mutex> lock(mailbox_mutex);
                pending_frame.reset();
                processing_running = true;
                mailbox_dropped_frames = 0;
                mailbox_dequeued_frames = 0;
                mailbox_max_depth = 0;
                mailbox_wait_time_ms = 0.0;
                mailbox_max_wait_ms = 0.0;
            }
            try {
                processing_thread = std::thread(&Impl::ProcessingLoop, this);
            } catch (const std::exception &error) {
                std::lock_guard<std::mutex> lock(mailbox_mutex);
                processing_running = false;
                LOG_ERROR("Failed to create UVC H.264 processing thread: {}", error.what());
                return false;
            }
            LOG_INFO("UVC H.264 processing thread started with latest-frame mailbox capacity 1");
            return true;
        }

        void StopProcessing() {
            const bool had_thread = processing_thread.joinable();
            {
                std::lock_guard<std::mutex> lock(mailbox_mutex);
                processing_running = false;
                pending_frame.reset();
            }
            mailbox_condition.notify_all();
            if (had_thread) {
                processing_thread.join();
                LOG_INFO("UVC H.264 processing thread stopped");
            }
        }

        void QueueFrame(uvc::UvcFramePtr frame) {
            {
                std::lock_guard<std::mutex> lock(mailbox_mutex);
                if (!processing_running) {
                    return;
                }
                if (pending_frame) {
                    ++mailbox_dropped_frames;
                }
                pending_frame = PendingFrame{std::move(frame), std::chrono::steady_clock::now()};
                mailbox_max_depth = std::max<size_t>(mailbox_max_depth, 1);
            }
            mailbox_condition.notify_one();
        }

        void ProcessingLoop() {
            while (true) {
                PendingFrame queued_frame;
                {
                    std::unique_lock<std::mutex> lock(mailbox_mutex);
                    mailbox_condition.wait(lock, [this] { return !processing_running || pending_frame.has_value(); });
                    if (!processing_running) {
                        break;
                    }
                    queued_frame = std::move(*pending_frame);
                    pending_frame.reset();
                    const double wait_ms = std::chrono::duration<double, std::milli>(
                                                   std::chrono::steady_clock::now() - queued_frame.enqueued_at)
                                                   .count();
                    ++mailbox_dequeued_frames;
                    mailbox_wait_time_ms += wait_ms;
                    mailbox_max_wait_ms = std::max(mailbox_max_wait_ms, wait_ms);
                }

                try {
                    if (queued_frame.frame) {
                        EncodeFrame(queued_frame.frame);
                    }
                } catch (const std::exception &error) {
                    LOG_ERROR("UVC H.264 processing threw an exception: {}", error.what());
                } catch (...) {
                    LOG_ERROR("UVC H.264 processing threw an unknown exception");
                }
            }
        }

        MailboxStatistics GetMailboxStatistics() {
            std::lock_guard<std::mutex> lock(mailbox_mutex);
            MailboxStatistics statistics;
            statistics.dropped_frames = mailbox_dropped_frames;
            statistics.dequeued_frames = mailbox_dequeued_frames;
            statistics.current_depth = pending_frame ? 1 : 0;
            statistics.max_depth = mailbox_max_depth;
            statistics.average_wait_ms = mailbox_dequeued_frames > 0
                                                 ? mailbox_wait_time_ms / mailbox_dequeued_frames
                                                 : 0.0;
            statistics.max_wait_ms = mailbox_max_wait_ms;
            return statistics;
        }

        void EncodeFrame(const uvc::UvcFramePtr &input) {
            if (!input) {
                return;
            }
            const auto frame_started = std::chrono::steady_clock::now();
            uvc::VdecFrame decoded;
            uvc::VdecTiming vdec_timing;
            uvc::Nv12Frame nv12;
            uvc::RgaTiming rga_timing;
            if (!hardware_decoder.Decode(input, &decoded, &vdec_timing) ||
                !nv12_converter.Convert(decoded, &nv12, &rga_timing)) {
                ++decode_errors;
                return;
            }
            const auto &layout = nv12.Layout();
            VIDEO_FRAME_INFO_S frame{};
            frame.stVFrame.pMbBlk = nv12.Block();
            frame.stVFrame.u32Width = config.capture.width;
            frame.stVFrame.u32Height = config.capture.height;
            frame.stVFrame.u32VirWidth = layout.u32VirWidth;
            frame.stVFrame.u32VirHeight = layout.u32VirHeight;
            frame.stVFrame.enField = VIDEO_FIELD_FRAME;
            frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
            frame.stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
            frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
            frame.stVFrame.enDynamicRange = DYNAMIC_RANGE_SDR8;
            frame.stVFrame.enColorGamut = COLOR_GAMUT_BT601;
            frame.stVFrame.u32TimeRef = input->sequence;
            frame.stVFrame.u64PTS = input->timestamp_us;
            const auto send_started = std::chrono::steady_clock::now();
            const RK_S32 result = RK_MPI_VENC_SendFrame(config.venc_channel, &frame, 1000);
            const auto finished = std::chrono::steady_clock::now();
            if (result != RK_SUCCESS) {
                ++send_errors;
                LOG_WARN("RK_MPI_VENC_SendFrame failed for UVC frame {}: {:#x}", input->sequence, result);
                return;
            }
            decode_time_ms += vdec_timing.send_ms + vdec_timing.get_ms;
            max_decode_time_ms = std::max(max_decode_time_ms, vdec_timing.send_ms + vdec_timing.get_ms);
            rga_time_ms += rga_timing.convert_ms;
            max_rga_time_ms = std::max(max_rga_time_ms, rga_timing.convert_ms);
            mb_get_time_ms += rga_timing.mb_get_ms;
            max_mb_get_time_ms = std::max(max_mb_get_time_ms, rga_timing.mb_get_ms);
            const double venc_ms = std::chrono::duration<double, std::milli>(finished - send_started).count();
            venc_send_time_ms += venc_ms;
            max_venc_send_time_ms = std::max(max_venc_send_time_ms, venc_ms);
            const double total_ms = std::chrono::duration<double, std::milli>(finished - frame_started).count();
            total_time_ms += total_ms;
            max_total_time_ms = std::max(max_total_time_ms, total_ms);
            ++frames_sent;
            const auto now = finished;
            if (frames_sent == 1) {
                statistics_started = now;
            } else if (frames_sent % 100 == 0) {
                const double seconds = std::chrono::duration<double>(now - statistics_started).count();
                const auto mailbox = GetMailboxStatistics();
                LOG_INFO("UVC hardware H.264 input: frames={}, fps={:.2f}, vdec_ms(avg/max)={:.2f}/{:.2f}, "
                         "rga_ms(avg/max)={:.3f}/{:.3f}, mb_get_ms(avg/max)={:.3f}/{:.3f}, "
                         "venc_send_ms(avg/max)={:.2f}/{:.2f}, total_ms(avg/max)={:.2f}/{:.2f}, "
                         "decode_errors={}, send_errors={}, mailbox_dequeued={}, mailbox_drops={}, "
                         "mailbox_depth(current/max)={}/{}, mailbox_wait_ms(avg/max)={:.2f}/{:.2f}",
                         frames_sent, seconds > 0.0 ? (frames_sent - 1) / seconds : 0.0,
                         decode_time_ms / frames_sent, max_decode_time_ms, rga_time_ms / frames_sent,
                         max_rga_time_ms, mb_get_time_ms / frames_sent, max_mb_get_time_ms,
                         venc_send_time_ms / frames_sent, max_venc_send_time_ms,
                         total_time_ms / frames_sent, max_total_time_ms, decode_errors, send_errors,
                         mailbox.dequeued_frames, mailbox.dropped_frames, mailbox.current_depth,
                         mailbox.max_depth, mailbox.average_wait_ms, mailbox.max_wait_ms);
            }
            return;
        }

        UvcH264Config config;
        uvc::UvcProducer capture;
        EncodedStreamDispatcher dispatcher;

        uvc::MjpegVdecDecoder hardware_decoder;
        uvc::RgaNv12Converter nv12_converter;
        MB_PIC_CAL_S buffer_layout{};
        bool mpi_initialized = false;
        bool venc_initialized = false;
        std::atomic<bool> accept_frames{false};

        std::mutex mailbox_mutex;
        std::condition_variable mailbox_condition;
        std::optional<PendingFrame> pending_frame;
        std::thread processing_thread;
        bool processing_running = false;
        uint64_t mailbox_dropped_frames = 0;
        uint64_t mailbox_dequeued_frames = 0;
        size_t mailbox_max_depth = 0;
        double mailbox_wait_time_ms = 0.0;
        double mailbox_max_wait_ms = 0.0;

        uint64_t frames_sent = 0;
        uint64_t decode_errors = 0;
        uint64_t send_errors = 0;
        double decode_time_ms = 0.0;
        double max_decode_time_ms = 0.0;
        double rga_time_ms = 0.0;
        double max_rga_time_ms = 0.0;
        double mb_get_time_ms = 0.0;
        double max_mb_get_time_ms = 0.0;
        double venc_send_time_ms = 0.0;
        double max_venc_send_time_ms = 0.0;
        double total_time_ms = 0.0;
        double max_total_time_ms = 0.0;
        std::chrono::steady_clock::time_point statistics_started;
    };

    UvcH264Producer::UvcH264Producer(UvcH264Config config) :
        config_(std::move(config)),
        shared_config_{static_cast<int>(config_.capture.fps), config_.bitrate_kbps},
        impl_(std::make_unique<Impl>(config_)) {}

    UvcH264Producer::~UvcH264Producer() { Deinit(); }

    int UvcH264Producer::Init() {
        if (initialized_.load()) {
            return 0;
        }
        if (config_.input_buffer_count < 2 || config_.venc_channel < 0 || config_.bitrate_kbps <= 0) {
            LOG_ERROR("Invalid UVC H.264 configuration");
            return -1;
        }
        if (impl_->capture.Init() != 0 || impl_->InitMpi() != 0) {
            impl_->Cleanup();
            return -1;
        }
        initialized_.store(true);
        LOG_INFO("UVC H.264 producer initialized: {}x{} @ {} fps", config_.capture.width,
                 config_.capture.height, config_.capture.fps);
        return 0;
    }

    int UvcH264Producer::Deinit() {
        Stop();
        impl_->Cleanup();
        initialized_.store(false);
        return 0;
    }

    bool UvcH264Producer::Start() {
        if (!initialized_.load()) {
            LOG_ERROR("UVC H.264 producer is not initialized");
            return false;
        }
        if (running_.load()) {
            return true;
        }
        impl_->ResetStatistics();
        if (!impl_->dispatcher.Start(config_.venc_channel)) {
            return false;
        }
        if (!impl_->StartProcessing()) {
            impl_->dispatcher.Stop();
            return false;
        }
        impl_->accept_frames.store(true);
        if (!impl_->capture.Start()) {
            impl_->accept_frames.store(false);
            impl_->StopProcessing();
            impl_->dispatcher.Stop();
            return false;
        }
        running_.store(true);
        LOG_INFO("UVC H.264 producer started");
        return true;
    }

    void UvcH264Producer::Stop() {
        impl_->accept_frames.store(false);
        impl_->capture.Stop();
        impl_->StopProcessing();
        impl_->dispatcher.Stop();
        if (running_.exchange(false)) {
            LOG_INFO("UVC H.264 producer stopped");
        }
    }

    void UvcH264Producer::RegisterStreamConsumer(const std::string &name, StreamCallback callback,
                                                 StreamConsumerType type, int queue_size) {
        (void) queue_size;
        impl_->dispatcher.RegisterConsumer(name, std::move(callback), type);
    }

    void UvcH264Producer::ClearStreamConsumers() { impl_->dispatcher.ClearConsumers(); }

    std::unique_ptr<IMediaProducer> CreateUvcH264Producer(const UvcH264Config &config) {
        return std::make_unique<UvcH264Producer>(config);
    }

} // namespace media
