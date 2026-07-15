/**
 * @file media_manager.cpp
 * @brief 媒体管理器实现
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#define LOG_TAG "MediaMgr"

#include "media_manager.h"
#include "common/logger.h"
#include "simple_ipc/simple_ipc_config.h"
#include "simple_ipc/simple_ipc_producer.h"
#include "uvc/uvc_h264_config.h"
#include "uvc/uvc_h264_producer.h"

#include <chrono>
#include <cstdlib>

namespace media {

    // ============================================================================
    // 单例实现
    // ============================================================================

    MediaManager &MediaManager::Instance() {
        static MediaManager instance;
        return instance;
    }

    MediaManager::~MediaManager() { Deinit(); }

    // ============================================================================
    // 生命周期管理
    // ============================================================================

    int MediaManager::Init(ProducerMode mode, const ProducerConfig &config) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (initialized_) {
            LOG_WARN("Media manager already initialized");
            return 0;
        }

        config_ = config;
        current_mode_ = mode;

        LOG_INFO("Initializing media manager with mode: {}", ProducerModeToString(mode));

        // 创建生产者实例
        producer_ = CreateProducerInstance(mode);
        if (!producer_) {
            LOG_ERROR("Failed to create producer for mode: {}", ProducerModeToString(mode));
            return -1;
        }

        // 初始化生产者
        if (producer_->Init() != 0) {
            LOG_ERROR("Failed to initialize producer");
            producer_.reset();
            return -1;
        }

        initialized_ = true;
        LOG_INFO("Media manager initialized successfully");
        return 0;
    }

    int MediaManager::Deinit() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            return 0;
        }

        if (producer_) {
            producer_->Stop();
            producer_->Deinit();
            producer_.reset();
        }

        initialized_ = false;
        manager_running_ = false;
        LOG_INFO("Media manager deinitialized");
        return 0;
    }

    bool MediaManager::Start() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_ || !producer_) {
            LOG_ERROR("Media manager not initialized");
            return false;
        }

        // 注册流消费者
        ReregisterConsumers();

        // 启动生产者
        if (!producer_->Start()) {
            LOG_ERROR("Failed to start producer");
            return false;
        }

        manager_running_ = true;

        LOG_INFO("Media manager started with mode: {}", ProducerModeToString(current_mode_));
        return true;
    }

    void MediaManager::Stop() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (producer_) {
            producer_->Stop();
        }

        manager_running_ = false;

        LOG_INFO("Media manager stopped");
    }

    bool MediaManager::IsRunning() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return producer_ && producer_->IsRunning();
    }

    // ============================================================================
    // 模式切换
    // ============================================================================

    int MediaManager::SwitchMode(ProducerMode mode) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            LOG_ERROR("Media manager not initialized");
            return -1;
        }

        if (mode == current_mode_) {
            LOG_DEBUG("Already in {} mode", ProducerModeToString(mode));
            return 0;
        }

        ProducerMode old_mode = current_mode_;

        LOG_INFO(">>> Mode switch: {} -> {} (cold start)", ProducerModeToString(old_mode), ProducerModeToString(mode));

        auto start_time = std::chrono::steady_clock::now();

        // 1. 停止当前生产者
        bool was_running = manager_running_;
        if (was_running && producer_) {
            producer_->Stop();
        }

        // 2. 销毁当前生产者（触发析构函数，释放硬件资源）
        if (producer_) {
            producer_->Deinit();
            producer_.reset();
            LOG_DEBUG("Old producer destroyed, hardware resources released");
        }

        // 3. 创建新生产者
        producer_ = CreateProducerInstance(mode);
        if (!producer_) {
            LOG_ERROR("Failed to create new producer, reverting...");
            producer_ = CreateProducerInstance(old_mode);
            if (producer_) {
                producer_->Init();
                ReregisterConsumers();
                if (was_running)
                    producer_->Start();
            }
            return -1;
        }

        // 4. 初始化新生产者
        if (producer_->Init() != 0) {
            LOG_ERROR("Failed to initialize new producer, reverting...");
            producer_.reset();
            producer_ = CreateProducerInstance(old_mode);
            if (producer_) {
                producer_->Init();
                ReregisterConsumers();
                if (was_running)
                    producer_->Start();
            }
            return -1;
        }

        current_mode_ = mode;
        mode_switch_count_++;

        // 5. 重新注册流消费者
        ReregisterConsumers();

        // 6. 如果之前在运行，重新启动
        if (was_running) {
            producer_->Start();
        }

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        LOG_INFO("<<< Mode switch completed in {}ms (switch count: {})", duration.count(), mode_switch_count_);

        // 7. 通知回调
        if (mode_switch_callback_) {
            mode_switch_callback_(old_mode, mode);
        }

        return 0;
    }

    // ============================================================================
    // 配置接口
    // ============================================================================

    int MediaManager::SetResolution(simple_ipc::Resolution preset) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (current_mode_ != ProducerMode::SimpleIPC) {
            LOG_WARN("SetResolution ignored: only valid in SimpleIPC mode (current: {})",
                     ProducerModeToString(current_mode_));
            return -1;
        }

        sipc_resolution_ = preset;

        if (!initialized_ || !producer_) {
            return 0; // 记录下来，等 Init() 时使用
        }

        bool was_running = producer_->IsRunning();
        if (was_running)
            producer_->Stop();
        producer_->Deinit();

        auto *sipc = dynamic_cast<SimpleIPCProducer *>(producer_.get());
        if (sipc) {
            sipc->SetResolution(preset);
        }

        if (producer_->Init() != 0) {
            LOG_ERROR("Failed to reinitialize SimpleIPC with new resolution");
            return -1;
        }

        ReregisterConsumers();

        if (was_running)
            producer_->Start();

        LOG_INFO("SimpleIPC resolution changed to preset {}", static_cast<int>(preset));
        return 0;
    }

    int MediaManager::SetResolution(uvc::Resolution preset) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (current_mode_ != ProducerMode::UvcH264) {
            LOG_WARN("UVC SetResolution ignored: current mode is {}", ProducerModeToString(current_mode_));
            return -1;
        }
        if (uvc_resolution_ == preset) {
            return 0;
        }

        const uvc::Resolution old_preset = uvc_resolution_;
        if (!initialized_ || !producer_) {
            uvc_resolution_ = preset;
            return 0;
        }

        const bool was_running = manager_running_;
        const auto rebuild = [this, was_running](uvc::Resolution target) {
            if (producer_) {
                producer_->Stop();
                producer_->Deinit();
                producer_.reset();
            }

            uvc_resolution_ = target;
            producer_ = CreateProducerInstance(ProducerMode::UvcH264);
            if (!producer_ || producer_->Init() != 0) {
                return false;
            }

            ReregisterConsumers();
            return !was_running || producer_->Start();
        };

        if (rebuild(preset)) {
            const auto resolution = uvc::ResolutionConfig::FromPreset(preset);
            LOG_INFO("UVC resolution changed to {}x{}", resolution.width, resolution.height);
            return 0;
        }

        LOG_ERROR("Failed to initialize requested UVC resolution; restoring previous preset");
        if (!rebuild(old_preset)) {
            LOG_ERROR("Failed to restore previous UVC resolution");
            if (producer_) {
                producer_->Stop();
                producer_->Deinit();
                producer_.reset();
            }
            uvc_resolution_ = old_preset;
            initialized_ = false;
            manager_running_ = false;
        }
        return -1;
    }

    int MediaManager::SetFrameRate(int fps) {
        std::lock_guard<std::mutex> lock(mutex_);

        fps = std::max(1, std::min(fps, 30));
        config_.framerate = fps;

        return 0;
    }

    // ============================================================================
    // 流消费者管理
    // ============================================================================

    void MediaManager::RegisterStreamConsumer(const std::string &name, StreamCallback callback, StreamConsumerType type,
                                              int queue_size) {

        std::lock_guard<std::mutex> lock(mutex_);

        if (manager_running_) {
            LOG_WARN("Cannot register stream consumer '{}' while media manager is running", name);
            return;
        }

        // 保存到列表
        consumers_.push_back({name, callback, type, queue_size});

        // 如果已有生产者，直接注册
        if (producer_) {
            producer_->RegisterStreamConsumer(name, callback, type, queue_size);
        }

        LOG_DEBUG("Stream consumer registered: {}", name);
    }

    void MediaManager::ClearStreamConsumers() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (manager_running_) {
            LOG_WARN("Cannot clear stream consumers while media manager is running");
            return;
        }

        consumers_.clear();
        if (producer_) {
            producer_->ClearStreamConsumers();
        }
    }

    void MediaManager::ReregisterConsumers() {
        if (!producer_)
            return;

        producer_->ClearStreamConsumers();
        for (const auto &c: consumers_) {
            producer_->RegisterStreamConsumer(c.name, c.callback, c.type, c.queue_size);
        }

        LOG_DEBUG("Reregistered {} stream consumers", consumers_.size());
    }

    // ============================================================================
    // 回调设置
    // ============================================================================

    void MediaManager::SetModeSwitchCallback(ModeSwitchCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_switch_callback_ = std::move(callback);
    }

    // ============================================================================
    // 状态查询
    // ============================================================================

    const char *MediaManager::GetCurrentTypeName() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (producer_) {
            return producer_->GetTypeName();
        }
        return "None";
    }

    // ============================================================================
    // 内部辅助
    // ============================================================================

    std::unique_ptr<IMediaProducer> MediaManager::CreateProducerInstance(ProducerMode mode) {
        switch (mode) {
            case ProducerMode::SimpleIPC: {
                SimpleIPCConfig sipc_config;
                sipc_config.framerate = config_.framerate;
                sipc_config.bitrate_kbps = config_.bitrate_kbps;
                sipc_config.resolution = sipc_resolution_;
                return CreateSimpleIPCProducer(sipc_config);
            }
            case ProducerMode::UvcH264: {
                UvcH264Config uvc_config;
                const auto resolution = uvc::ResolutionConfig::FromPreset(uvc_resolution_);
                if (const char *device = std::getenv("AIPC_UVC_DEVICE")) {
                    uvc_config.capture.device = device;
                }
                uvc_config.capture.width = resolution.width;
                uvc_config.capture.height = resolution.height;
                uvc_config.capture.fps = config_.framerate;
                uvc_config.bitrate_kbps = config_.bitrate_kbps;
                uvc_config.gop = config_.framerate;
                return CreateUvcH264Producer(uvc_config);
            }
            
            default:
                LOG_ERROR("Unknown producer mode: {}", static_cast<int>(mode));
                return nullptr;
        }
    }

} // namespace media
