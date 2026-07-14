#pragma once

#include "common/asio_context.h"
#include "common/logger.h"
#include "common/media_buffer.h"
#include "i_media_producer.h"

#include <atomic>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace media {

    inline std::shared_ptr<spdlog::logger> GetEncodedDispatcherLogger() {
        return LogManager::GetLogger("EncodedDispatcher");
    }

    class EncodedStreamDispatcher {
    public:
        struct ConsumerInfo {
            std::string name;
            StreamCallback callback;
            StreamConsumerType type;
        };

        ~EncodedStreamDispatcher() { Stop(); }

        void RegisterConsumer(const std::string &name, StreamCallback callback, StreamConsumerType type) {
            if (running_.load()) {
                SPDLOG_LOGGER_WARN(GetEncodedDispatcherLogger(),
                                   "Cannot register stream consumer '{}' while dispatcher is running", name);
                return;
            }
            consumers_.push_back({name, std::move(callback), type});
            SPDLOG_LOGGER_INFO(GetEncodedDispatcherLogger(), "Registered stream consumer: {} (type={})", name,
                               type == StreamConsumerType::AsyncIO ? "AsyncIO" : "Direct");
        }

        void ClearConsumers() {
            if (running_.load()) {
                SPDLOG_LOGGER_WARN(GetEncodedDispatcherLogger(),
                                   "Cannot clear stream consumers while dispatcher is running");
                return;
            }
            consumers_.clear();
        }

        bool Start(int venc_channel) {
            if (running_.load()) {
                return true;
            }
            venc_channel_ = venc_channel;
            running_.store(true);
            try {
                fetch_thread_ = std::thread(&EncodedStreamDispatcher::FetchLoop, this);
            } catch (const std::exception &error) {
                running_.store(false);
                SPDLOG_LOGGER_ERROR(GetEncodedDispatcherLogger(),
                                    "Failed to start encoded stream dispatcher: {}", error.what());
                return false;
            }
            SPDLOG_LOGGER_INFO(GetEncodedDispatcherLogger(),
                               "Encoded stream dispatcher started for VENC channel {}", venc_channel_);
            return true;
        }

        void Stop() {
            running_.store(false);
            if (fetch_thread_.joinable()) {
                fetch_thread_.join();
                SPDLOG_LOGGER_INFO(GetEncodedDispatcherLogger(), "Encoded stream dispatcher stopped");
            }
        }

        bool IsRunning() const { return running_.load(); }

    private:
        void FetchLoop() {
            uint64_t frame_count = 0;
            uint64_t consecutive_errors = 0;
            while (running_.load()) {
                RK_S32 last_error = 0;
                auto stream = acquire_encoded_stream(venc_channel_, 1000, &last_error);
                if (!stream) {
                    ++consecutive_errors;
                    if (running_.load() && consecutive_errors > 3 && consecutive_errors % 10 == 0) {
                        SPDLOG_LOGGER_WARN(GetEncodedDispatcherLogger(),
                                           "VENC consecutive errors: {}, last: {:#x}", consecutive_errors,
                                           last_error);
                    }
                    continue;
                }

                ++frame_count;
                consecutive_errors = 0;
                if (frame_count <= 5 || frame_count % 300 == 0) {
                    SPDLOG_LOGGER_DEBUG(GetEncodedDispatcherLogger(), "Encoded frame #{}, size={} bytes",
                                        frame_count, stream->pstPack ? stream->pstPack->u32Len : 0);
                }

                for (const auto &consumer : consumers_) {
                    if (!consumer.callback) {
                        continue;
                    }
                    if (consumer.type == StreamConsumerType::AsyncIO) {
                        PostToIo([callback = consumer.callback, stream] { callback(stream); });
                    } else {
                        consumer.callback(stream);
                    }
                }
            }
            SPDLOG_LOGGER_DEBUG(GetEncodedDispatcherLogger(), "Encoded fetch loop exited, total frames: {}",
                                frame_count);
        }

        std::vector<ConsumerInfo> consumers_;
        std::atomic<bool> running_{false};
        std::thread fetch_thread_;
        int venc_channel_ = 0;
    };

} // namespace media
