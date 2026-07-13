/**
 * @file file_service.cpp
 * @brief 文件保存服务实现
 *
 * @author 好软，好温暖
 * @date 2026-02-04
 */

#define LOG_TAG "file_service"

#include "file_service.h"
#include "common/logger.h"

// ============================================================================
// FileService 实现
// ============================================================================

FileService::FileService(const FileServiceConfig& config)
    : config_(config) {
    
    // 创建 MP4 录制器
    mp4_recorder_ = std::make_unique<Mp4Recorder>(config_.mp4Config);
    
    LOG_INFO("FileService created");
}

FileService::~FileService() {
    Stop();
    LOG_INFO("FileService destroyed");
}

void FileService::Start() {
    if (running_) {
        LOG_WARN("FileService already running");
        return;
    }
    
    running_ = true;
    LOG_INFO("FileService started");
}

void FileService::Stop() {
    if (!running_) {
        return;
    }
    
    LOG_INFO("Stopping FileService...");
    
    // 停止录制
    if (mp4_recorder_ && mp4_recorder_->IsRecording()) {
        mp4_recorder_->StopRecording();
    }
    
    running_ = false;
    LOG_INFO("FileService stopped");
}

// ============================================================================
// 录制控制接口
// ============================================================================

bool FileService::StartRecording(const std::string& filename) {
    if (!mp4_recorder_) {
        LOG_ERROR("MP4 recorder not initialized");
        return false;
    }
    
    return mp4_recorder_->StartRecording(filename);
}

void FileService::StopRecording() {
    if (mp4_recorder_) {
        mp4_recorder_->StopRecording();
    }
}

bool FileService::IsRecording() const {
    return mp4_recorder_ && mp4_recorder_->IsRecording();
}

std::string FileService::GetCurrentRecordPath() const {
    if (mp4_recorder_) {
        return mp4_recorder_->GetCurrentFilePath();
    }
    return "";
}

Mp4Recorder::Stats FileService::GetRecordStats() const {
    if (mp4_recorder_) {
        return mp4_recorder_->GetStats();
    }
    return {};
}

// ============================================================================
// 流消费者接口
// ============================================================================

void FileService::OnEncodedStream(const EncodedStreamPtr& stream) {
    // 如果正在录制，写入帧
    if (mp4_recorder_ && mp4_recorder_->IsRecording()) {
        mp4_recorder_->WriteFrame(stream);
    }
}
