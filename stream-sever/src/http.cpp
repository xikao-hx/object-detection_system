/**
 * @file http.cpp
 * @brief HTTP API 模块实现
 *
 * 使用新的 MediaManager (Producer-based) 架构
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#define LOG_TAG "http"

#include "http.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "common/logger.h"
#include "media_distribution/file/file_service.h"
#include "media_distribution/rtsp/rtsp_service.h"
#include "media_distribution/webrtc/webrtc_service.h"
#include "media_producer/media_manager.h"
#include "media_producer/simple_ipc/simple_ipc_config.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

// ============================================================================
// 模型文件目录（上传和管理）
// ============================================================================
static const std::string MODEL_DIR = "../model";
static const size_t MAX_UPLOAD_SIZE = 50 * 1024 * 1024; // 50MB

// ============================================================================
// 辅助函数
// ============================================================================

/**
 * @brief 净化文件名，移除路径穿越字符
 */
static std::string SanitizeFilename(const std::string &name) {
    std::string result;
    for (char c: name) {
        if (c == '/' || c == '\\' || c == '\0')
            continue;
        result += c;
    }
    // 防止 ".." 路径穿越
    if (result.find("..") != std::string::npos) {
        return "";
    }
    return result;
}

/**
 * @brief 生成 JSON 响应
 */
static std::string json_response(bool success, const std::string &message, const json &data = nullptr) {
    json response;
    response["success"] = success;
    response["message"] = message;
    if (!data.is_null()) {
        response["data"] = data;
    }
    return response.dump();
}

// ============================================================================
// HttpApi 实现
// ============================================================================

HttpApi::HttpApi() = default;

HttpApi::~HttpApi() { Stop(); }

bool HttpApi::Init(const HttpApiConfig &config, const StreamConfig &stream_config) {
    LOG_INFO("初始化 HTTP API: {}:{}", config.host, config.port);

    config_ = config;
    stream_config_ = stream_config;

    // 创建 HTTP 服务器
    server_ = std::make_unique<HttpServer>();

    HttpServerConfig http_config;
    http_config.host = config.host;
    http_config.port = config.port;
    http_config.static_dir = config.static_dir;
    http_config.static_mount = "/";
    http_config.thread_pool_size = config.thread_pool_size;

    if (!server_->Init(http_config)) {
        LOG_ERROR("HTTP 服务器初始化失败");
        return false;
    }

    // 设置 API 路由
    SetupRoutes();

    LOG_INFO("HTTP API 初始化完成");
    return true;
}

bool HttpApi::Start() {
    if (!server_) {
        LOG_ERROR("HTTP 服务器未初始化");
        return false;
    }

    if (!server_->Start()) {
        LOG_ERROR("HTTP 服务器启动失败");
        return false;
    }

    LOG_INFO("HTTP API 服务已启动: {}:{}", config_.host, config_.port);
    return true;
}

void HttpApi::Stop() {
    if (server_) {
        server_->Stop();
        LOG_INFO("HTTP API 服务已停止");
    }
}

bool HttpApi::IsRunning() const { return server_ && server_->IsRunning(); }

void HttpApi::SetupRoutes() {
    if (!server_) {
        return;
    }

    // ========================================================================
    // 系统状态 API
    // ========================================================================
    server_->Get("/api/status", [this](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        json data;

        // RTSP 状态
        data["rtsp"]["enabled"] = stream_config_.enable_rtsp;
        if (mgr && mgr->GetRtspService()) {
            data["rtsp"]["valid"] = mgr->GetRtspService()->IsValid();
            data["rtsp"]["running"] = mgr->GetRtspService()->IsRunning();
        }

        // WebRTC 状态
        data["webrtc"]["enabled"] = stream_config_.enable_webrtc;
        if (mgr && mgr->GetWebRTCService()) {
            data["webrtc"]["running"] = mgr->GetWebRTCService()->IsRunning();
        }

        // 录制状态
        data["recording"]["enabled"] = stream_config_.enable_file;
        if (mgr && mgr->GetFileService()) {
            auto *fs = mgr->GetFileService();
            data["recording"]["active"] = fs->IsRecording();
            data["recording"]["output_dir"] = stream_config_.mp4_config.outputDir;
        }

        // 媒体生产者状态
        auto &media_mgr = media::MediaManager::Instance();
        data["producer"]["mode"] = media::ProducerModeToString(media_mgr.GetCurrentMode());
        data["producer"]["running"] = media_mgr.IsRunning();

        res.set_content(json_response(true, "ok", data), "application/json");
    });

    // ========================================================================
    // RTSP 状态 API
    // ========================================================================
    server_->Get("/api/rtsp/status", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetRtspService()) {
            res.set_content(json_response(false, "RTSP not available"), "application/json");
            return;
        }

        auto *rtsp = mgr->GetRtspService();
        auto stats = rtsp->GetStats();
        json data;
        data["valid"] = rtsp->IsValid();
        data["running"] = rtsp->IsRunning();
        data["url"] = rtsp->GetUrl();
        data["frames_sent"] = stats.framesSent;
        data["bytes_sent"] = stats.bytesSent;
        res.set_content(json_response(true, "ok", data), "application/json");
    });

    // ========================================================================
    // RTSP 控制 API
    // ========================================================================
    server_->Post("/api/rtsp/start", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetRtspService()) {
            res.set_content(json_response(false, "RTSP not available"), "application/json");
            return;
        }

        auto *rtsp = mgr->GetRtspService();
        if (rtsp->IsRunning()) {
            res.set_content(json_response(true, "RTSP already running"), "application/json");
            return;
        }

        if (rtsp->Start()) {
            res.set_content(json_response(true, "RTSP started"), "application/json");
        } else {
            res.set_content(json_response(false, "Failed to start RTSP"), "application/json");
        }
    });

    server_->Post("/api/rtsp/stop", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetRtspService()) {
            res.set_content(json_response(false, "RTSP not available"), "application/json");
            return;
        }

        auto *rtsp = mgr->GetRtspService();
        if (!rtsp->IsRunning()) {
            res.set_content(json_response(true, "RTSP already stopped"), "application/json");
            return;
        }

        rtsp->Stop();
        res.set_content(json_response(true, "RTSP stopped"), "application/json");
    });

    // ========================================================================
    // WebRTC 状态和控制 API
    // ========================================================================
    server_->Get("/api/webrtc/status", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }

        auto *webrtc = mgr->GetWebRTCService();
        json data;
        data["running"] = webrtc->IsRunning();
        res.set_content(json_response(true, "ok", data), "application/json");
    });

    server_->Post("/api/webrtc/start", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }

        auto *webrtc = mgr->GetWebRTCService();
        if (webrtc->IsRunning()) {
            res.set_content(json_response(true, "WebRTC already running"), "application/json");
            return;
        }

        if (webrtc->Start()) {
            res.set_content(json_response(true, "WebRTC started"), "application/json");
        } else {
            res.set_content(json_response(false, "Failed to start WebRTC"), "application/json");
        }
    });

    server_->Post("/api/webrtc/stop", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }

        auto *webrtc = mgr->GetWebRTCService();
        if (!webrtc->IsRunning()) {
            res.set_content(json_response(true, "WebRTC already stopped"), "application/json");
            return;
        }

        webrtc->Stop();
        res.set_content(json_response(true, "WebRTC stopped"), "application/json");
    });

    // ========================================================================
    // WebRTC HTTP 信令 API
    // ========================================================================
    server_->Post("/api/webrtc/offer", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }

        auto *webrtc = mgr->GetWebRTCService();
        if (!webrtc->IsRunning()) {
            res.set_content(json_response(false, "WebRTC service not running"), "application/json");
            return;
        }

        try {
            std::string offer = webrtc->CreateOfferForHttp();
            if (offer.empty()) {
                res.set_content(json_response(false, "Failed to create offer"), "application/json");
                return;
            }

            json data;
            data["sdp"] = offer;
            data["type"] = "offer";
            res.set_content(json_response(true, "ok", data), "application/json");
        } catch (const std::exception &e) {
            res.set_content(json_response(false, std::string("Error: ") + e.what()), "application/json");
        }
    });

    server_->Post("/api/webrtc/answer", [](const HttpRequest &req, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }

        auto *webrtc = mgr->GetWebRTCService();

        try {
            json body = json::parse(req.body);
            std::string sdp = body.value("sdp", "");

            if (sdp.empty()) {
                res.set_content(json_response(false, "Missing SDP"), "application/json");
                return;
            }

            if (webrtc->SetAnswerFromHttp(sdp)) {
                res.set_content(json_response(true, "Answer set"), "application/json");
            } else {
                res.set_content(json_response(false, "Failed to set answer"), "application/json");
            }
        } catch (const json::exception &e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    server_->Post("/api/webrtc/ice", [](const HttpRequest &req, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }

        auto *webrtc = mgr->GetWebRTCService();

        try {
            json body = json::parse(req.body);
            std::string candidate = body.value("candidate", "");
            std::string mid = body.value("sdpMid", "0");

            if (candidate.empty()) {
                // 空候选表示 ICE 收集完成
                res.set_content(json_response(true, "ICE gathering complete"), "application/json");
                return;
            }

            if (webrtc->AddIceCandidateFromHttp(candidate, mid)) {
                res.set_content(json_response(true, "ICE candidate added"), "application/json");
            } else {
                res.set_content(json_response(false, "Failed to add ICE candidate"), "application/json");
            }
        } catch (const json::exception &e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    server_->Get("/api/webrtc/candidates", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetWebRTCService()) {
            res.set_content(json_response(false, "WebRTC not available"), "application/json");
            return;
        }

        auto *webrtc = mgr->GetWebRTCService();
        auto candidates = webrtc->GetLocalIceCandidates();

        json data = json::array();
        for (const auto &[candidate, mid]: candidates) {
            json c;
            c["candidate"] = candidate;
            c["sdpMid"] = mid;
            data.push_back(c);
        }

        res.set_content(json_response(true, "ok", data), "application/json");
    });

    // ========================================================================
    // 录制状态和控制 API
    // ========================================================================
    server_->Get("/api/record/status", [this](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetFileService()) {
            res.set_content(json_response(false, "Recording not available"), "application/json");
            return;
        }

        auto *fs = mgr->GetFileService();
        json data;
        data["enabled"] = stream_config_.enable_file;
        data["active"] = fs->IsRecording();
        data["output_dir"] = stream_config_.mp4_config.outputDir;

        if (fs->IsRecording()) {
            auto stats = fs->GetRecordStats();
            data["stats"]["frames_written"] = stats.frames_written;
            data["stats"]["bytes_written"] = stats.bytes_written;
            data["stats"]["duration_sec"] = stats.duration_sec;
        }

        res.set_content(json_response(true, "ok", data), "application/json");
    });

    server_->Post("/api/record/start", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetFileService()) {
            res.set_content(json_response(false, "Recording not available"), "application/json");
            return;
        }

        auto *fs = mgr->GetFileService();
        if (fs->IsRecording()) {
            res.set_content(json_response(true, "Recording already active"), "application/json");
            return;
        }

        if (fs->StartRecording()) {
            res.set_content(json_response(true, "Recording started"), "application/json");
        } else {
            res.set_content(json_response(false, "Failed to start recording"), "application/json");
        }
    });

    server_->Post("/api/record/stop", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto *mgr = GetStreamManager();
        if (!mgr || !mgr->GetFileService()) {
            res.set_content(json_response(false, "Recording not available"), "application/json");
            return;
        }

        auto *fs = mgr->GetFileService();
        if (!fs->IsRecording()) {
            res.set_content(json_response(true, "Recording already stopped"), "application/json");
            return;
        }

        fs->StopRecording();
        res.set_content(json_response(true, "Recording stopped"), "application/json");
    });

    // ========================================================================
    // 生产者模式 API（基于新的 MediaManager）
    // ========================================================================
    server_->Get("/api/producer/status", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto &mgr = media::MediaManager::Instance();

        json data;
        data["mode"] = media::ProducerModeToString(mgr.GetCurrentMode());
        data["running"] = mgr.IsRunning();
        data["available_modes"] = json::array({"simple_ipc"});

        res.set_content(json_response(true, "ok", data), "application/json");
    });

    server_->Post("/api/producer/switch", [](const HttpRequest &req, HttpResponse &res) {
        try {
            json body = json::parse(req.body);
            std::string mode_str = body.value("mode", "simple_ipc");

            LOG_INFO("Producer mode switch requested: {}", mode_str);

            if (mode_str == "simple_ipc") {
                json data;
                data["mode"] = media::ProducerModeToString(media::ProducerMode::SimpleIPC);
                res.set_content(json_response(true, "Already in requested mode", data), "application/json");
                return;
            }

            res.set_content(json_response(false, "Unsupported producer mode"), "application/json");

        } catch (const json::exception &e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    // ========================================================================
    // AI API（前端兼容接口）
    // ========================================================================
    server_->Get("/api/ai/status", [](const HttpRequest & /*req*/, HttpResponse &res) {
        json data;
        data["has_model"] = false;
        data["model_type"] = "none";

        // stats: AI 统计信息（当前未实现详细统计，返回占位数据）
        json stats;
        stats["frames_processed"] = 0;
        stats["avg_inference_ms"] = 0;
        stats["total_detections"] = 0;
        data["stats"] = stats;

        res.set_content(json_response(true, "ok", data), "application/json");
    });

    server_->Post("/api/ai/switch", [](const HttpRequest &req, HttpResponse &res) {
        try {
            json body = json::parse(req.body);
            std::string model_str = body.value("model", "none");

            LOG_INFO("AI model switch requested: {}", model_str);

            if (model_str == "none" || model_str == "simple_ipc" || model_str.empty()) {
                json data;
                data["model"] = "none";
                res.set_content(json_response(true, "AI model disabled", data), "application/json");
            } else {
                res.set_content(json_response(false, "Unsupported AI model"), "application/json");
            }

        } catch (const json::exception &e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    // ========================================================================
    // Pipeline API（前端兼容接口）
    // ========================================================================
    server_->Get("/api/pipeline/status", [](const HttpRequest & /*req*/, HttpResponse &res) {
        auto &mgr = media::MediaManager::Instance();
        auto cfg = mgr.GetConfig();
        auto sipc_res = mgr.GetSIPCResolution();
        auto res_cfg = media::simple_ipc::ResolutionConfig::FromPreset(sipc_res);
        res_cfg.framerate = cfg.framerate;

        json data;

        // mode: parallel (纯 IPC)
        data["mode"] = "parallel";

        // resolution 信息
        json resolution;
        if (sipc_res == media::simple_ipc::Resolution::R_1080P) {
            resolution["preset"] = "1080p";
        } else if (sipc_res == media::simple_ipc::Resolution::R_720P) {
            resolution["preset"] = "720p";
        } else {
            resolution["preset"] = "480p";
        }
        resolution["width"] = res_cfg.width;
        resolution["height"] = res_cfg.height;
        resolution["framerate"] = res_cfg.framerate;
        data["resolution"] = resolution;

        // 状态信息
        data["initialized"] = mgr.IsInitialized();
        data["streaming"] = mgr.IsRunning();

        // 可用分辨率
        data["available_resolutions"] = json::array({"1080p", "720p", "480p"});
        data["note"] = "";

        res.set_content(json_response(true, "ok", data), "application/json");
    });

    server_->Post("/api/pipeline/resolution", [](const HttpRequest &req, HttpResponse &res) {
        try {
            json body = json::parse(req.body);
            std::string preset_str = body.value("resolution", "1080p");

            LOG_INFO("Resolution switch requested: {}", preset_str);

            // 解析分辨率预设
            media::simple_ipc::Resolution target_res;
            if (preset_str == "720p") {
                target_res = media::simple_ipc::Resolution::R_720P;
            } else if (preset_str == "480p") {
                target_res = media::simple_ipc::Resolution::R_480P;
            } else {
                target_res = media::simple_ipc::Resolution::R_1080P;
            }

            auto &mgr = media::MediaManager::Instance();

            if (mgr.GetCurrentMode() != media::ProducerMode::SimpleIPC) {
                res.set_content(json_response(false, "Resolution switching is only available in SimpleIPC mode"),
                                "application/json");
                return;
            }

            if (mgr.GetSIPCResolution() == target_res) {
                auto res_cfg = media::simple_ipc::ResolutionConfig::FromPreset(target_res);
                json data;
                data["resolution"] = preset_str;
                data["width"] = res_cfg.width;
                data["height"] = res_cfg.height;
                res.set_content(json_response(true, "Already using requested resolution", data), "application/json");
                return;
            }

            // 切换分辨率（需要重新初始化）
            if (mgr.SetResolution(target_res) != 0) {
                res.set_content(json_response(false, "Failed to switch resolution"), "application/json");
                return;
            }

            auto res_cfg = media::simple_ipc::ResolutionConfig::FromPreset(target_res);
            json data;
            data["resolution"] = preset_str;
            data["width"] = res_cfg.width;
            data["height"] = res_cfg.height;
            res.set_content(json_response(true, "Resolution switched", data), "application/json");

        } catch (const json::exception &e) {
            res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
        }
    });

    // ========================================================================
    // 模型文件管理 API
    // ========================================================================

    // 列出所有模型文件
    server_->Get("/api/model/list", [](const HttpRequest & /*req*/, HttpResponse &res) {
        namespace fs = std::filesystem;
        json models = json::array();

        try {
            for (const auto &entry: fs::directory_iterator(MODEL_DIR)) {
                if (!entry.is_regular_file())
                    continue;
                auto ext = entry.path().extension().string();
                // 小写化扩展名
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".rknn" || ext == ".txt") {
                    json item;
                    item["name"] = entry.path().filename().string();
                    item["size"] = entry.file_size();
                    item["type"] = ext.substr(1); // "rknn" or "txt"
                    models.push_back(item);
                }
            }
        } catch (const fs::filesystem_error &e) {
            res.set_content(json_response(false, std::string("Failed to list models: ") + e.what()),
                            "application/json");
            return;
        }

        res.set_content(json_response(true, "ok", models), "application/json");
    });

    // 上传模型文件
    server_->Post("/api/model/upload", [](const HttpRequest &req, HttpResponse &res) {
        if (!req.form.has_file("file")) {
            res.set_content(json_response(false, "No file in request"), "application/json");
            return;
        }

        const auto &file = req.form.get_file("file");
        std::string filename = SanitizeFilename(file.filename);

        if (filename.empty()) {
            res.set_content(json_response(false, "Invalid filename"), "application/json");
            return;
        }

        // 仅允许 .rknn 和 .txt 后缀
        std::string ext = filename.substr(filename.find_last_of('.') + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != "rknn" && ext != "txt") {
            res.set_content(json_response(false, "Only .rknn and .txt files are allowed"), "application/json");
            return;
        }

        // 文件大小限制
        if (file.content.size() > MAX_UPLOAD_SIZE) {
            res.set_content(json_response(false, "File too large (max 50MB)"), "application/json");
            return;
        }

        std::string path = MODEL_DIR + "/" + filename;
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) {
            res.set_content(json_response(false, "Failed to write file"), "application/json");
            return;
        }
        ofs.write(file.content.data(), static_cast<std::streamsize>(file.content.size()));
        ofs.close();

        json data;
        data["name"] = filename;
        data["size"] = file.content.size();
        LOG_INFO("Model file uploaded: {} ({} bytes)", filename, file.content.size());
        res.set_content(json_response(true, "File uploaded", data), "application/json");
    });

    // 删除模型文件
    server_->Delete(R"(/api/model/(.+))", [](const HttpRequest &req, HttpResponse &res) {
        std::string filename = SanitizeFilename(req.matches[1].str());

        if (filename.empty()) {
            res.set_content(json_response(false, "Invalid filename"), "application/json");
            return;
        }

        std::string path = MODEL_DIR + "/" + filename;
        if (!std::filesystem::exists(path)) {
            res.set_content(json_response(false, "File not found"), "application/json");
            return;
        }

        std::filesystem::remove(path);
        LOG_INFO("Model file deleted: {}", filename);
        res.set_content(json_response(true, "File deleted"), "application/json");
    });

    // ========================================================================
    // 注册模型列表 API
    // ========================================================================

    // 获取 C++ 注册的可用模型类型列表
    server_->Get("/api/models/registered", [](const HttpRequest & /*req*/, HttpResponse &res) {
        json list = json::array();
        res.set_content(json_response(true, "ok", list), "application/json");
    });

    LOG_INFO("HTTP API 路由配置完成");
}
