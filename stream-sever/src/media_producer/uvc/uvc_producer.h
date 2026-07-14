#pragma once

#include "uvc_config.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace media::uvc {

    struct UvcFrame {
        std::vector<uint8_t> data;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t sequence = 0;
        uint64_t timestamp_us = 0;
    };

    using UvcFramePtr = std::shared_ptr<const UvcFrame>;
    using UvcFrameCallback = std::function<void(UvcFramePtr)>;

    /** 返回首个满足 config 规格的 uvcvideo capture 节点，找不到时返回空字符串。 */
    std::string FindUvcDevice(const UvcConfig &config);

    class UvcProducer {
    public:
        explicit UvcProducer(UvcConfig config = {});
        ~UvcProducer();

        UvcProducer(const UvcProducer &) = delete;
        UvcProducer &operator=(const UvcProducer &) = delete;

        /** 回调只能在非运行状态设置。 */
        bool SetFrameCallback(UvcFrameCallback callback);

        int Init();
        int Deinit();
        bool Start();
        void Stop();

        bool IsInitialized() const;
        bool IsRunning() const;
        const UvcConfig &GetConfig() const;
        const std::string &GetDevicePath() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace media::uvc
