/**
 * @file ws_preview.cpp
 * @brief WebSocket H.264 裸流预览实现
 */

#include "ws_preview.h"
#include "common/h264_nal_parser.h"
#include "common/logger.h"

#include <rtc/websocketserver.hpp>
#include <rtc/websocket.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

#undef LOG_TAG
#define LOG_TAG "ws_preview"

// ============================================================================
// WsPreviewServer 实现
// ============================================================================

WsPreviewServer::WsPreviewServer(const WsPreviewConfig& config)
    : config_(config) {
    LOG_INFO("WebSocket 预览服务器创建: port={}", config_.port);
}

WsPreviewServer::~WsPreviewServer() {
    Stop();
    LOG_INFO("WebSocket 预览服务器销毁");
}

bool WsPreviewServer::Start() {
    if (running_.load()) {
        LOG_WARN("WebSocket 预览服务器已在运行");
        return true;
    }

    // 尝试手动创建 socket 测试
    LOG_INFO("尝试创建 WebSocket 服务器，端口: {}", config_.port);
    
    // 重试机制
    const int maxRetries = 3;
    const int retryDelayMs = 1000;

    for (int retry = 0; retry < maxRetries; ++retry) {
        try {
            // 创建 WebSocket 服务器配置
            rtc::WebSocketServer::Configuration config;
            config.port = config_.port;
            config.enableTls = false;  // 内网使用，不需要 TLS
            config.bindAddress = "0.0.0.0";  // 明确绑定 IPv4

            LOG_DEBUG("创建 WebSocketServer 对象...");
            // 创建服务器
            ws_server_ = std::make_unique<rtc::WebSocketServer>(config);

            // 设置客户端连接回调
            ws_server_->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
                OnClientConnected(ws);
            });

            running_.store(true);
            LOG_INFO("WebSocket 预览服务器启动成功: port={}", ws_server_->port());
            return true;

        } catch (const std::exception& e) {
            if (retry < maxRetries - 1) {
                LOG_WARN("WebSocket 端口 {} 创建失败，重试中 ({}/{})...: {}", 
                         config_.port, retry + 1, maxRetries, e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
            } else {
                LOG_ERROR("启动 WebSocket 服务器失败 (重试 {} 次): {}", maxRetries, e.what());
            }
        }
    }

    return false;
}

void WsPreviewServer::Stop() {
    if (!running_.load()) {
        return;
    }

    LOG_INFO("停止 WebSocket 预览服务器...");
    running_.store(false);

    // 先拿出所有客户端 shared_ptr，释放锁后再 close
    // 避免死锁：close() 可能同步触发 onClosed 回调，回调中会获取 clients_mutex_
    std::vector<std::shared_ptr<rtc::WebSocket>> clients_to_close;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_to_close.swap(clients_);
    }
    
    // 在锁外关闭所有客户端
    for (auto& ws : clients_to_close) {
        if (ws) {
            try {
                ws->close();
            } catch (...) {}
        }
    }
    clients_to_close.clear();  // 释放所有 shared_ptr

    // 停止服务器
    if (ws_server_) {
        ws_server_->stop();
        ws_server_.reset();
    }

    LOG_INFO("WebSocket 预览服务器已停止, 总计发送: {} 帧, {} 字节",
             frames_sent_.load(), bytes_sent_.load());
}

uint16_t WsPreviewServer::GetPort() const {
    return ws_server_ ? ws_server_->port() : 0;
}

size_t WsPreviewServer::GetClientCount() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    // 统计活跃的客户端数量
    return std::count_if(clients_.begin(), clients_.end(),
        [](const std::shared_ptr<rtc::WebSocket>& ws) {
            return ws && ws->isOpen();
        });
}

void WsPreviewServer::OnClientConnected(std::shared_ptr<rtc::WebSocket> ws) {
    LOG_INFO("WebSocket 客户端连接, path={}", ws->path().value_or("(none)"));

    // 先添加到客户端列表（使用 shared_ptr 保持连接存活）
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        
        // 清理已断开的连接
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                [](const std::shared_ptr<rtc::WebSocket>& s) {
                    return !s || !s->isOpen();
                }),
            clients_.end());

        // 检查是否超过最大连接数
        if (static_cast<int>(clients_.size()) >= config_.max_clients) {
            LOG_WARN("达到最大客户端数量限制 ({}), 拒绝新连接", config_.max_clients);
            ws->close();
            return;
        }

        // 存入 shared_ptr，增加引用计数，保持连接存活
        clients_.push_back(ws);
        LOG_INFO("当前客户端数量: {}", clients_.size());
    }

    // 设置 onOpen 回调 - WebSocket 完全就绪后再发送数据
    ws->onOpen([this, weak_ws = std::weak_ptr<rtc::WebSocket>(ws)]() {
        LOG_INFO("WebSocket 客户端已就绪");
        if (auto ws = weak_ws.lock()) {
            // 发送缓存的 SPS/PPS
            SendSpsPps(ws);
        }
    });

    // 设置消息回调（可用于接收客户端控制命令）
    ws->onMessage([](auto data) {
        // 目前不处理客户端消息
        if (std::holds_alternative<std::string>(data)) {
            LOG_DEBUG("收到客户端消息: {}", std::get<std::string>(data));
        }
    });

    // 关键：onClosed 中必须从 clients_ 移除 shared_ptr，否则引用计数永远不为0
    ws->onClosed([this, weak_ws = std::weak_ptr<rtc::WebSocket>(ws)]() {
        LOG_INFO("WebSocket 客户端断开");
        
        // 锁定并移除对应的 shared_ptr
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                [&weak_ws](const std::shared_ptr<rtc::WebSocket>& s) {
                    // 比较指针地址是否相同，或者连接已关闭
                    auto target = weak_ws.lock();
                    return !s || !s->isOpen() || (target && s == target);
                }),
            clients_.end());
    });

    ws->onError([](std::string error) {
        LOG_WARN("WebSocket 错误: {}", error);
    });
}

void WsPreviewServer::SendVideoFrame(const uint8_t* data, size_t size, uint64_t /*timestamp*/) {
    if (!running_.load() || !data || size == 0) {
        return;
    }

    // 提取 SPS/PPS（如果有）
    ExtractSpsPps(data, size);

    // 获取活跃客户端（拷贝一份以减少锁持有时间）
    std::vector<std::shared_ptr<rtc::WebSocket>> active_clients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        // 直接拷贝 shared_ptr 列表，增加引用计数
        active_clients = clients_;
    }

    if (active_clients.empty()) {
        return;
    }

    // 发送给所有客户端
    for (auto& ws : active_clients) {
        if (ws && ws->isOpen()) {
            try {
                ws->send(reinterpret_cast<const std::byte*>(data), size);
            } catch (const std::exception& e) {
                LOG_DEBUG("发送视频帧失败: {}", e.what());
            }
        }
    }

    frames_sent_++;
    bytes_sent_ += size * active_clients.size();
}

void WsPreviewServer::ExtractSpsPps(const uint8_t* data, size_t size) {
    h264::NalUnit sps;
    h264::NalUnit pps;
    if (!h264::FindSpsPps(data, size, &sps, &pps)) {
        return;
    }

    std::lock_guard<std::mutex> lock(sps_pps_mutex_);
    if (sps.annexb_start && sps.annexb_size > 0) {
        cached_sps_.assign(sps.annexb_start, sps.annexb_start + sps.annexb_size);
        LOG_DEBUG("缓存 SPS: {} 字节", sps.annexb_size);
    }
    if (pps.annexb_start && pps.annexb_size > 0) {
        cached_pps_.assign(pps.annexb_start, pps.annexb_start + pps.annexb_size);
        LOG_DEBUG("缓存 PPS: {} 字节", pps.annexb_size);
    }
}

bool WsPreviewServer::IsKeyframe(const uint8_t* data, size_t size) const {
    return h264::IsKeyframe(data, size);
}

void WsPreviewServer::SendSpsPps(std::shared_ptr<rtc::WebSocket> ws) {
    std::lock_guard<std::mutex> lock(sps_pps_mutex_);

    if (cached_sps_.empty() || cached_pps_.empty()) {
        LOG_DEBUG("SPS/PPS 尚未缓存，等待下一个关键帧");
        return;
    }

    try {
        // 先发送 SPS
        ws->send(reinterpret_cast<const std::byte*>(cached_sps_.data()), cached_sps_.size());
        // 再发送 PPS
        ws->send(reinterpret_cast<const std::byte*>(cached_pps_.data()), cached_pps_.size());
        LOG_DEBUG("已发送缓存的 SPS/PPS 给新客户端");
    } catch (const std::exception& e) {
        LOG_WARN("发送 SPS/PPS 失败: {}", e.what());
    }
}

void WsPreviewServer::OnEncodedStream(const EncodedStreamPtr& stream) {
    if (!stream || !stream->pstPack) {
        return;
    }

    // 使用 media_buffer.h 中的辅助函数获取数据地址（零拷贝）
    const uint8_t* data = static_cast<const uint8_t*>(get_stream_vir_addr(stream));
    uint32_t len = get_stream_length(stream);
    uint64_t pts = get_stream_pts(stream);

    if (data && len > 0) {
        SendVideoFrame(data, len, pts);
    }
}
