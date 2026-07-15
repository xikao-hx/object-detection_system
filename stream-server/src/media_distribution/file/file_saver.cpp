/**
 * @file file_saver.cpp
 * @brief 文件保存器实现 - MP4 录制和 JPEG 拍照
 *
 * @author 好软，好温暖
 * @date 2026-01-31
 */

#define LOG_TAG "file"

#include "file_saver.h"
#include "common/h264_nal_parser.h"
#include "common/logger.h"
#include "common/media_buffer.h"

#include "rk_mpi_mb.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_tde.h"
#include "rk_mpi_cal.h"

#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <chrono>
#include <thread>

// FFmpeg 头文件
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
}

// ============================================================================
// 辅助函数
// ============================================================================

static std::string GenerateTimestampFilename(const std::string& prefix) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time);
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%s_%04d%02d%02d_%02d%02d%02d",
             prefix.c_str(),
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

static bool EnsureDirectory(const std::string& dir) {
    struct stat st;
    if (stat(dir.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    
    if (mkdir(dir.c_str(), 0755) == 0) {
        return true;
    }
    
    LOG_ERROR("Failed to create directory: {}", dir);
    return false;
}

// ============================================================================
// Mp4Recorder 实现
// ============================================================================

Mp4Recorder::Mp4Recorder(const Mp4RecordConfig& config)
    : config_(config) {
    EnsureDirectory(config_.outputDir);
    LOG_INFO("Mp4Recorder created, output dir: {}", config_.outputDir);
}

Mp4Recorder::~Mp4Recorder() {
    StopRecording();
    LOG_INFO("Mp4Recorder destroyed");
}

std::string Mp4Recorder::GenerateFilename() {
    return GenerateTimestampFilename(config_.filenamePrefix);
}

bool Mp4Recorder::CreateOutputFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    AVFormatContext* ofmt_ctx = nullptr;
    if (avformat_alloc_output_context2(&ofmt_ctx, nullptr, nullptr, filepath.c_str()) < 0) {
        LOG_ERROR("Could not create output context for: {}", filepath);
        return false;
    }
    
    AVCodecID codec_id = (config_.codecType == 12) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec* codec = avcodec_find_encoder(codec_id);
    if (!codec) {
        LOG_ERROR("Codec not found: {}", (config_.codecType == 12) ? "H.265" : "H.264");
        avformat_free_context(ofmt_ctx);
        return false;
    }
    
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        LOG_ERROR("Failed to allocate codec context");
        avformat_free_context(ofmt_ctx);
        return false;
    }
    
    codec_ctx->codec_id = codec_id;
    codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    codec_ctx->bit_rate = 3000000;
    codec_ctx->width = config_.width;
    codec_ctx->height = config_.height;
    codec_ctx->time_base = AVRational{1, config_.fps};
    codec_ctx->framerate = AVRational{config_.fps, 1};
    codec_ctx->gop_size = config_.gopSize;
    codec_ctx->max_b_frames = 0;
    codec_ctx->pix_fmt = AV_PIX_FMT_NV12;
    
    AVStream* video_st = avformat_new_stream(ofmt_ctx, codec);
    if (!video_st) {
        LOG_ERROR("Failed to create video stream");
        avcodec_free_context(&codec_ctx);
        avformat_free_context(ofmt_ctx);
        return false;
    }
    
    if (avcodec_parameters_from_context(video_st->codecpar, codec_ctx) < 0) {
        LOG_ERROR("Failed to copy codec parameters to stream");
        avcodec_free_context(&codec_ctx);
        avformat_free_context(ofmt_ctx);
        return false;
    }
    
    video_st->codecpar->codec_tag = 0;
    video_st->time_base = AVRational{1, config_.fps};
    
    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt_ctx->pb, filepath.c_str(), AVIO_FLAG_WRITE) < 0) {
            LOG_ERROR("Could not open output file: {}", filepath);
            avcodec_free_context(&codec_ctx);
            avformat_free_context(ofmt_ctx);
            return false;
        }
    }
    
    // 不在这里写入 header，等待第一个关键帧设置 extradata 后再写
    
    format_ctx_ = ofmt_ctx;
    codec_ctx_ = codec_ctx;
    current_file_path_ = filepath;
    first_pts_ = 0;
    stats_ = Stats{};
    header_written_ = false;  // 标记 header 尚未写入
    
    LOG_INFO("Created output file (waiting for keyframe): {}", filepath);
    return true;
}

void Mp4Recorder::CloseOutputFile() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (format_ctx_) {
        AVFormatContext* ofmt_ctx = static_cast<AVFormatContext*>(format_ctx_);
        
        // 只有在 header 已写入时才写 trailer
        if (header_written_) {
            av_write_trailer(ofmt_ctx);
        }
        
        if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ofmt_ctx->pb);
        }
        
        avformat_free_context(ofmt_ctx);
        format_ctx_ = nullptr;
    }
    
    if (codec_ctx_) {
        AVCodecContext* ctx = static_cast<AVCodecContext*>(codec_ctx_);
        avcodec_free_context(&ctx);
        codec_ctx_ = nullptr;
    }
    
    if (!current_file_path_.empty()) {
        LOG_INFO("Closed output file: {}, {} frames, {:.2f} sec", 
                 current_file_path_, stats_.frames_written, stats_.duration_sec);
        current_file_path_.clear();
    }
    
    extradata_set_ = false;
    header_written_ = false;
}

bool Mp4Recorder::SetExtradataFromStream(const uint8_t* data, size_t size) {
    // 从 H.264 Annex B 流中提取 SPS 和 PPS
    // H.264 NAL 类型：7=SPS, 8=PPS

    h264::NalUnit sps;
    h264::NalUnit pps;
    if (!h264::FindSpsPps(data, size, &sps, &pps)) {
        LOG_WARN("SPS or PPS not found in stream");
        return false;
    }
    LOG_DEBUG("Found SPS: {} bytes", sps.size);
    LOG_DEBUG("Found PPS: {} bytes", pps.size);
    
    // 构造 AVCC 格式的 extradata
    // AVCC 格式：
    // 1 byte: version (1)
    // 1 byte: profile
    // 1 byte: compatibility
    // 1 byte: level
    // 1 byte: 0xFC | (lengthSizeMinusOne & 0x03)  -> 通常是 0xFF (4字节长度)
    // 1 byte: 0xE0 | numSPS  -> 通常是 0xE1 (1个SPS)
    // 2 bytes: SPS length (big endian)
    // SPS data
    // 1 byte: numPPS -> 通常是 0x01
    // 2 bytes: PPS length (big endian)
    // PPS data
    
    size_t extradata_size = 6 + 2 + sps.size + 1 + 2 + pps.size;
    uint8_t* extradata = static_cast<uint8_t*>(av_malloc(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!extradata) {
        LOG_ERROR("Failed to allocate extradata");
        return false;
    }
    memset(extradata, 0, extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    
    uint8_t* p = extradata;
    *p++ = 1;                          // version
    *p++ = sps.start[1];               // profile
    *p++ = sps.start[2];               // compatibility
    *p++ = sps.start[3];               // level
    *p++ = 0xFF;                       // 4 bytes NAL length
    *p++ = 0xE1;                       // 1 SPS
    *p++ = (sps.size >> 8) & 0xFF;     // SPS length high byte
    *p++ = sps.size & 0xFF;            // SPS length low byte
    memcpy(p, sps.start, sps.size);
    p += sps.size;
    *p++ = 1;                          // 1 PPS
    *p++ = (pps.size >> 8) & 0xFF;     // PPS length high byte
    *p++ = pps.size & 0xFF;            // PPS length low byte
    memcpy(p, pps.start, pps.size);
    
    // 设置到 codecpar
    AVFormatContext* ofmt_ctx = static_cast<AVFormatContext*>(format_ctx_);
    AVStream* video_stream = ofmt_ctx->streams[0];
    
    // 释放旧的 extradata（如果有）
    if (video_stream->codecpar->extradata) {
        av_free(video_stream->codecpar->extradata);
    }
    
    video_stream->codecpar->extradata = extradata;
    video_stream->codecpar->extradata_size = extradata_size;
    
    LOG_INFO("Set H.264 extradata: SPS={} bytes, PPS={} bytes", sps.size, pps.size);
    return true;
}

bool Mp4Recorder::StartRecording(const std::string& filename) {
    if (state_ != RecordState::kIdle) {
        LOG_WARN("Recording already in progress");
        return false;
    }
    
    std::string fname = filename.empty() ? GenerateFilename() : filename;
    std::string filepath = config_.outputDir + "/" + fname + ".mp4";
    
    if (!CreateOutputFile(filepath)) {
        return false;
    }
    
    state_ = RecordState::kRecording;
    LOG_INFO("Started recording to: {}", filepath);
    return true;
}

void Mp4Recorder::StopRecording() {
    if (state_ != RecordState::kRecording) {
        return;
    }
    
    state_ = RecordState::kStopping;
    CloseOutputFile();
    state_ = RecordState::kIdle;
    
    LOG_INFO("Stopped recording");
}

bool Mp4Recorder::WriteFrame(const EncodedStreamPtr& stream) {
    if (state_ != RecordState::kRecording || !stream || !stream->pstPack) {
        return false;
    }
    
    void* data = get_stream_vir_addr(stream);
    if (!data || stream->pstPack->u32Len == 0) {
        LOG_WARN("Invalid frame data");
        return false;
    }
    
    bool is_keyframe = (stream->pstPack->DataType.enH264EType == H264E_NALU_IDRSLICE ||
                        stream->pstPack->DataType.enH264EType == H264E_NALU_ISLICE ||
                        stream->pstPack->DataType.enH265EType == H265E_NALU_IDRSLICE ||
                        stream->pstPack->DataType.enH265EType == H265E_NALU_ISLICE);
    
    // 等待第一个关键帧，从中提取 SPS/PPS 并写入 header
    if (!header_written_) {
        if (!is_keyframe) {
            // 跳过非关键帧，等待关键帧
            LOG_DEBUG("Waiting for keyframe to start recording...");
            return true;
        }
        
        // 从关键帧中提取 SPS/PPS 设置 extradata
        if (!SetExtradataFromStream(static_cast<uint8_t*>(data), stream->pstPack->u32Len)) {
            LOG_ERROR("Failed to extract SPS/PPS from keyframe");
            return false;
        }
        extradata_set_ = true;
        
        // 现在写入 header
        AVFormatContext* ofmt_ctx = static_cast<AVFormatContext*>(format_ctx_);
        if (avformat_write_header(ofmt_ctx, nullptr) < 0) {
            LOG_ERROR("Error writing header");
            return false;
        }
        header_written_ = true;
        first_pts_ = stream->pstPack->u64PTS;
        LOG_INFO("Header written, recording started from keyframe");
    }
    
    bool max_duration_reached = false;
    double duration_sec = 0.0;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!format_ctx_) {
            return false;
        }

        AVFormatContext* ofmt_ctx = static_cast<AVFormatContext*>(format_ctx_);
        AVStream* video_stream = ofmt_ctx->streams[0];

        int flags = 0;
        if (is_keyframe) {
            flags |= AV_PKT_FLAG_KEY;
        }

        uint64_t relative_pts = (stream->pstPack->u64PTS - first_pts_) / 1000;

        AVPacket packet = {};
        packet.data = static_cast<uint8_t*>(data);
        packet.size = stream->pstPack->u32Len;
        packet.pts = av_rescale_q(relative_pts, AVRational{1, 1000}, video_stream->time_base);
        packet.dts = packet.pts;
        packet.stream_index = video_stream->index;
        packet.duration = av_rescale_q(1, AVRational{1, config_.fps}, video_stream->time_base);
        packet.flags = flags;

        int ret = av_interleaved_write_frame(ofmt_ctx, &packet);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Error writing frame: {}", errbuf);
            return false;
        }

        stats_.frames_written++;
        stats_.bytes_written += stream->pstPack->u32Len;
        stats_.duration_sec = static_cast<double>(relative_pts) / 1000.0;

        duration_sec = stats_.duration_sec;
        max_duration_reached = config_.maxDurationSec > 0 && duration_sec >= config_.maxDurationSec;
    }

    if (max_duration_reached) {
        LOG_INFO("Max recording duration reached ({:.2f}s >= {}s), stopping recording",
                 duration_sec, config_.maxDurationSec);
        StopRecording();
    }
    
    return true;
}
