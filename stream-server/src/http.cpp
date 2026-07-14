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

    RegisterSystemRoutes();
    RegisterRtspRoutes();
    RegisterWebrtcRoutes();
    RegisterRecordRoutes();
    RegisterProducerRoutes();
    RegisterAiRoutes();
    RegisterPipelineRoutes();
    RegisterModelRoutes();

    LOG_INFO("HTTP API 路由配置完成");
}

void HttpApi::RegisterSystemRoutes() {
    server_->Get("/api/status", [this](const HttpRequest &req, HttpResponse &res) {
        HandleStatus(req, res);
    });
}

void HttpApi::RegisterRtspRoutes() {
    server_->Get("/api/rtsp/status", [this](const HttpRequest &req, HttpResponse &res) {
        HandleRtspStatus(req, res);
    });
    server_->Post("/api/rtsp/start", [this](const HttpRequest &req, HttpResponse &res) {
        HandleRtspStart(req, res);
    });
    server_->Post("/api/rtsp/stop", [this](const HttpRequest &req, HttpResponse &res) {
        HandleRtspStop(req, res);
    });
}

void HttpApi::RegisterWebrtcRoutes() {
    server_->Get("/api/webrtc/status", [this](const HttpRequest &req, HttpResponse &res) {
        HandleWebrtcStatus(req, res);
    });
    server_->Post("/api/webrtc/start", [this](const HttpRequest &req, HttpResponse &res) {
        HandleWebrtcStart(req, res);
    });
    server_->Post("/api/webrtc/stop", [this](const HttpRequest &req, HttpResponse &res) {
        HandleWebrtcStop(req, res);
    });
    server_->Post("/api/webrtc/offer", [this](const HttpRequest &req, HttpResponse &res) {
        HandleWebrtcOffer(req, res);
    });
    server_->Post("/api/webrtc/answer", [this](const HttpRequest &req, HttpResponse &res) {
        HandleWebrtcAnswer(req, res);
    });
    server_->Post("/api/webrtc/ice", [this](const HttpRequest &req, HttpResponse &res) {
        HandleWebrtcIce(req, res);
    });
    server_->Get("/api/webrtc/candidates", [this](const HttpRequest &req, HttpResponse &res) {
        HandleWebrtcCandidates(req, res);
    });
}

void HttpApi::RegisterRecordRoutes() {
    server_->Get("/api/record/status", [this](const HttpRequest &req, HttpResponse &res) {
        HandleRecordStatus(req, res);
    });
    server_->Post("/api/record/start", [this](const HttpRequest &req, HttpResponse &res) {
        HandleRecordStart(req, res);
    });
    server_->Post("/api/record/stop", [this](const HttpRequest &req, HttpResponse &res) {
        HandleRecordStop(req, res);
    });
}

void HttpApi::RegisterProducerRoutes() {
    server_->Get("/api/producer/status", [this](const HttpRequest &req, HttpResponse &res) {
        HandleProducerStatus(req, res);
    });
    server_->Post("/api/producer/switch", [this](const HttpRequest &req, HttpResponse &res) {
        HandleProducerSwitch(req, res);
    });
}

void HttpApi::RegisterAiRoutes() {
    server_->Get("/api/ai/status", [this](const HttpRequest &req, HttpResponse &res) {
        HandleAiStatus(req, res);
    });
    server_->Post("/api/ai/switch", [this](const HttpRequest &req, HttpResponse &res) {
        HandleAiSwitch(req, res);
    });
}

void HttpApi::RegisterPipelineRoutes() {
    server_->Get("/api/pipeline/status", [this](const HttpRequest &req, HttpResponse &res) {
        HandlePipelineStatus(req, res);
    });
    server_->Post("/api/pipeline/resolution", [this](const HttpRequest &req, HttpResponse &res) {
        HandlePipelineResolution(req, res);
    });
}

void HttpApi::RegisterModelRoutes() {
    server_->Get("/api/model/list", [this](const HttpRequest &req, HttpResponse &res) {
        HandleModelList(req, res);
    });
    server_->Post("/api/model/upload", [this](const HttpRequest &req, HttpResponse &res) {
        HandleModelUpload(req, res);
    });
    server_->Delete(R"(/api/model/(.+))", [this](const HttpRequest &req, HttpResponse &res) {
        HandleModelDelete(req, res);
    });
    server_->Get("/api/models/registered", [this](const HttpRequest &req, HttpResponse &res) {
        HandleRegisteredModels(req, res);
    });
}

void HttpApi::HandleStatus(const HttpRequest& /*req*/, HttpResponse& res) {
    auto *mgr = GetStreamManager();
    json data;

    data["rtsp"]["enabled"] = stream_config_.enable_rtsp;
    if (mgr && mgr->GetRtspService()) {
        data["rtsp"]["valid"] = mgr->GetRtspService()->IsValid();
        data["rtsp"]["running"] = mgr->GetRtspService()->IsRunning();
    }

    data["webrtc"]["enabled"] = stream_config_.enable_webrtc;
    if (mgr && mgr->GetWebRTCService()) {
        data["webrtc"]["running"] = mgr->GetWebRTCService()->IsRunning();
    }

    data["recording"]["enabled"] = stream_config_.enable_file;
    if (mgr && mgr->GetFileService()) {
        auto *fs = mgr->GetFileService();
        data["recording"]["active"] = fs->IsRecording();
        data["recording"]["output_dir"] = stream_config_.mp4_config.outputDir;
    }

    auto &media_mgr = media::MediaManager::Instance();
    data["producer"]["mode"] = media::ProducerModeToString(media_mgr.GetCurrentMode());
    data["producer"]["running"] = media_mgr.IsRunning();

    res.set_content(json_response(true, "ok", data), "application/json");
}

void HttpApi::HandleRtspStatus(const HttpRequest& /*req*/, HttpResponse& res) {
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
}

void HttpApi::HandleRtspStart(const HttpRequest& /*req*/, HttpResponse& res) {
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
}

void HttpApi::HandleRtspStop(const HttpRequest& /*req*/, HttpResponse& res) {
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
}

void HttpApi::HandleWebrtcStatus(const HttpRequest& /*req*/, HttpResponse& res) {
    auto *mgr = GetStreamManager();
    if (!mgr || !mgr->GetWebRTCService()) {
        res.set_content(json_response(false, "WebRTC not available"), "application/json");
        return;
    }

    auto *webrtc = mgr->GetWebRTCService();
    json data;
    data["running"] = webrtc->IsRunning();
    res.set_content(json_response(true, "ok", data), "application/json");
}

void HttpApi::HandleWebrtcStart(const HttpRequest& /*req*/, HttpResponse& res) {
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
}

void HttpApi::HandleWebrtcStop(const HttpRequest& /*req*/, HttpResponse& res) {
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
}

void HttpApi::HandleWebrtcOffer(const HttpRequest& /*req*/, HttpResponse& res) {
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
}

void HttpApi::HandleWebrtcAnswer(const HttpRequest& req, HttpResponse& res) {
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
}

void HttpApi::HandleWebrtcIce(const HttpRequest& req, HttpResponse& res) {
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
}

void HttpApi::HandleWebrtcCandidates(const HttpRequest& /*req*/, HttpResponse& res) {
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
}

void HttpApi::HandleRecordStatus(const HttpRequest& /*req*/, HttpResponse& res) {
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
}

void HttpApi::HandleRecordStart(const HttpRequest& /*req*/, HttpResponse& res) {
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
}

void HttpApi::HandleRecordStop(const HttpRequest& /*req*/, HttpResponse& res) {
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
}

void HttpApi::HandleProducerStatus(const HttpRequest& /*req*/, HttpResponse& res) {
    auto &mgr = media::MediaManager::Instance();

    json data;
    data["mode"] = media::ProducerModeToString(mgr.GetCurrentMode());
    data["running"] = mgr.IsRunning();
    data["available_modes"] = json::array({"simple_ipc", "uvc"});

    res.set_content(json_response(true, "ok", data), "application/json");
}

void HttpApi::HandleProducerSwitch(const HttpRequest& req, HttpResponse& res) {
    try {
        json body = json::parse(req.body);
        std::string mode_str = body.value("mode", "simple_ipc");

        LOG_INFO("Producer mode switch requested: {}", mode_str);

        media::ProducerMode target_mode;
        if (mode_str == "simple_ipc" || mode_str == "ipc") {
            target_mode = media::ProducerMode::SimpleIPC;
        } else if (mode_str == "uvc" || mode_str == "uvc_h264") {
            target_mode = media::ProducerMode::UvcH264;
        } else {
            res.set_content(json_response(false, "Unsupported producer mode"), "application/json");
            return;
        }

        auto &manager = media::MediaManager::Instance();
        if (manager.GetCurrentMode() == target_mode) {
            json data;
            data["mode"] = media::ProducerModeToString(target_mode);
            res.set_content(json_response(true, "Already in requested mode", data), "application/json");
            return;
        }

        if (manager.SwitchMode(target_mode) != 0) {
            res.set_content(json_response(false, "Failed to switch producer mode"), "application/json");
            return;
        }

        json data;
        data["mode"] = media::ProducerModeToString(target_mode);
        res.set_content(json_response(true, "Producer mode switched", data), "application/json");
    } catch (const json::exception &e) {
        res.set_content(json_response(false, std::string("Invalid JSON: ") + e.what()), "application/json");
    }
}

void HttpApi::HandleAiStatus(const HttpRequest& /*req*/, HttpResponse& res) {
    json data;
    data["has_model"] = false;
    data["model_type"] = "none";

    json stats;
    stats["frames_processed"] = 0;
    stats["avg_inference_ms"] = 0;
    stats["total_detections"] = 0;
    data["stats"] = stats;

    res.set_content(json_response(true, "ok", data), "application/json");
}

void HttpApi::HandleAiSwitch(const HttpRequest& req, HttpResponse& res) {
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
}

void HttpApi::HandlePipelineStatus(const HttpRequest& /*req*/, HttpResponse& res) {
    auto &mgr = media::MediaManager::Instance();
    auto cfg = mgr.GetConfig();
    if (mgr.GetCurrentMode() == media::ProducerMode::UvcH264) {
        json data;
        data["mode"] = "uvc";
        json resolution;
        resolution["preset"] = "uvc_stereo";
        resolution["width"] = 1280;
        resolution["height"] = 480;
        resolution["framerate"] = cfg.framerate;
        data["resolution"] = resolution;
        data["initialized"] = mgr.IsInitialized();
        data["streaming"] = mgr.IsRunning();
        data["available_resolutions"] = json::array({"1280x480"});
        data["note"] = "USB UVC stereo side-by-side input";
        res.set_content(json_response(true, "ok", data), "application/json");
        return;
    }
    auto sipc_res = mgr.GetSIPCResolution();
    auto res_cfg = media::simple_ipc::ResolutionConfig::FromPreset(sipc_res);
    res_cfg.framerate = cfg.framerate;

    json data;
    data["mode"] = "parallel";

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

    data["initialized"] = mgr.IsInitialized();
    data["streaming"] = mgr.IsRunning();
    data["available_resolutions"] = json::array({"1080p", "720p", "480p"});
    data["note"] = "";

    res.set_content(json_response(true, "ok", data), "application/json");
}

void HttpApi::HandlePipelineResolution(const HttpRequest& req, HttpResponse& res) {
    try {
        json body = json::parse(req.body);
        std::string preset_str = body.value("resolution", "1080p");

        LOG_INFO("Resolution switch requested: {}", preset_str);

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
}

void HttpApi::HandleModelList(const HttpRequest& /*req*/, HttpResponse& res) {
    namespace fs = std::filesystem;
    json models = json::array();

    try {
        for (const auto &entry: fs::directory_iterator(MODEL_DIR)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".rknn" || ext == ".txt") {
                json item;
                item["name"] = entry.path().filename().string();
                item["size"] = entry.file_size();
                item["type"] = ext.substr(1);
                models.push_back(item);
            }
        }
    } catch (const fs::filesystem_error &e) {
        res.set_content(json_response(false, std::string("Failed to list models: ") + e.what()),
                        "application/json");
        return;
    }

    res.set_content(json_response(true, "ok", models), "application/json");
}

void HttpApi::HandleModelUpload(const HttpRequest& req, HttpResponse& res) {
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

    std::string ext = filename.substr(filename.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != "rknn" && ext != "txt") {
        res.set_content(json_response(false, "Only .rknn and .txt files are allowed"), "application/json");
        return;
    }

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
}

void HttpApi::HandleModelDelete(const HttpRequest& req, HttpResponse& res) {
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
}

void HttpApi::HandleRegisteredModels(const HttpRequest& /*req*/, HttpResponse& res) {
    json list = json::array();
    res.set_content(json_response(true, "ok", list), "application/json");
}
