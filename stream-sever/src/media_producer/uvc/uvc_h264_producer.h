#pragma once

#include "../i_media_producer.h"
#include "uvc_h264_config.h"

#include <atomic>
#include <memory>

namespace media {

    class UvcH264Producer : public IMediaProducer {
    public:
        explicit UvcH264Producer(UvcH264Config config = {});
        ~UvcH264Producer() override;

        UvcH264Producer(const UvcH264Producer &) = delete;
        UvcH264Producer &operator=(const UvcH264Producer &) = delete;

        int Init() override;
        int Deinit() override;
        bool Start() override;
        void Stop() override;

        void RegisterStreamConsumer(const std::string &name, StreamCallback callback,
                                    StreamConsumerType type = StreamConsumerType::AsyncIO,
                                    int queue_size = 3) override;
        void ClearStreamConsumers() override;

        bool IsInitialized() const override { return initialized_.load(); }
        bool IsRunning() const override { return running_.load(); }
        const char *GetTypeName() const override { return "UVC-H264"; }
        const ProducerConfig &GetConfig() const override { return shared_config_; }
        const UvcH264Config &GetUvcH264Config() const { return config_; }

    private:
        struct Impl;

        UvcH264Config config_;
        ProducerConfig shared_config_;
        std::atomic<bool> initialized_{false};
        std::atomic<bool> running_{false};
        std::unique_ptr<Impl> impl_;
    };

} // namespace media
