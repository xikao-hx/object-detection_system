# 进度记录

## Step 1：安全性与配置修复
- 状态：已完成
- 开始时间：2026-07-01
- 完成时间：2026-07-01
- 修改文件：
  - `stream-sever/src/media_distribution/stream_manager.cpp`
  - `stream-sever/src/media_producer/simple_ipc/mpi_config.h`
  - `stream-sever/src/media_producer/simple_ipc/simple_ipc_producer.cpp`
  - `stream-sever/src/media_producer/media_manager.cpp`
  - `memory-bank/*`
- 完成内容：
  - `StreamManager::Stop()` 现在会停止 WebSocket 预览。
  - `venc_init()` 现在接收码率、帧率和 GOP 参数，不再硬编码 10 Mbps 和 30 fps。
  - `SimpleIPCProducer::InitMpi()` 会把 `SimpleIPCConfig` 中的码率/帧率传入 VENC 初始化。
  - `MediaManager` 的 const 方法现在直接锁已有的 mutable mutex，不再使用 `const_cast`。
- 验证：
  - `cmake --build stream-sever/build` 失败，原因是该目录没有 CMake cache。
  - `cmake --build stream-sever/build/Debug` 成功。

## Step 2：删除 service 全局单例死代码
- 状态：已完成
- 完成时间：2026-07-01
- 修改文件：
  - `stream-sever/src/media_distribution/rtsp/rtsp_service.h`
  - `stream-sever/src/media_distribution/rtsp/rtsp_service.cpp`
  - `stream-sever/src/media_distribution/file/file_service.h`
  - `stream-sever/src/media_distribution/file/file_service.cpp`
  - `stream-sever/src/media_distribution/webrtc/webrtc_service.h`
  - `stream-sever/src/media_distribution/webrtc/webrtc_service.cpp`
  - `stream-sever/src/media_distribution/wspreview/ws_preview.h`
  - `stream-sever/src/media_distribution/wspreview/ws_preview.cpp`
- 完成内容：
  - 删除各 service 自己维护的全局 `unique_ptr`。
  - 删除 `GetXxxService()` / `CreateXxxService()` / `DestroyXxxService()` 全局 helper。
  - 保留 `StreamManager` 作为唯一 service 所有者和访问入口。
- 验证：
  - `cmake --build stream-sever/build/Debug` 成功。

## Step 3：类型化 Stream Consumer 与运行时注册保护
- 状态：已完成
- 完成时间：2026-07-01
- 修改文件：
  - `stream-sever/src/main.cpp`
  - `stream-sever/src/media_distribution/rtsp/rtsp_service.h`
  - `stream-sever/src/media_distribution/rtsp/rtsp_service.cpp`
  - `stream-sever/src/media_distribution/file/file_service.h`
  - `stream-sever/src/media_distribution/webrtc/webrtc_service.h`
  - `stream-sever/src/media_distribution/webrtc/webrtc_service.cpp`
  - `stream-sever/src/media_distribution/wspreview/ws_preview.h`
  - `stream-sever/src/media_distribution/wspreview/ws_preview.cpp`
  - `stream-sever/src/media_producer/media_manager.cpp`
  - `stream-sever/src/media_producer/simple_ipc/simple_ipc_producer.cpp`
- 完成内容：
  - `main.cpp` 注册 consumer 时直接捕获 service 指针并调用成员函数。
  - 删除 service 层 `static StreamConsumer(EncodedStreamPtr, void*)` 路径。
  - `MediaManager` 和 `SimpleStreamDispatcher` 在运行中拒绝新增/清理 consumer。
  - shutdown 顺序改为先停止 `MediaManager`，再清除 consumer，符合运行中禁止变更规则。
- 验证：
  - `cmake --build stream-sever/build/Debug` 成功。

## Step 4：共享 H.264 NAL Parser
- 状态：已完成
- 完成时间：2026-07-01
- 修改文件：
  - `stream-sever/src/common/h264_nal_parser.h`
  - `stream-sever/src/media_distribution/file/file_saver.cpp`
  - `stream-sever/src/media_distribution/wspreview/ws_preview.cpp`
  - `stream-sever/src/media_distribution/webrtc/webrtc.cpp`
- 完成内容：
  - 新增 header-only H.264 Annex-B NAL parser。
  - `file_saver.cpp` 使用 common parser 提取不含 start code 的 SPS/PPS。
  - `ws_preview.cpp` 使用 common parser 缓存含 start code 的 SPS/PPS。
  - `webrtc.cpp` 使用 common parser 判断关键帧。
- 验证：
  - `cmake --build stream-sever/build/Debug` 成功。

## Step 5：录制时长自动停止
- 状态：已完成
- 完成时间：2026-07-01
- 修改文件：
  - `stream-sever/src/media_distribution/file/file_saver.cpp`
- 完成内容：
  - `maxDurationSec` 到期后会在当前帧写入完成后自动调用 `StopRecording()`。
  - 停止动作发生在写帧锁释放之后，避免递归加锁。
  - 日志会记录实际录制时长和配置上限。
- 验证：
  - `cmake --build stream-sever/build/Debug` 成功。

## Step 6：拆分 HTTP 路由
- 状态：已完成
- 完成时间：2026-07-01
- 修改文件：
  - `stream-sever/src/http.h`
  - `stream-sever/src/http.cpp`
- 完成内容：
  - `SetupRoutes()` 只保留路由分组注册。
  - 新增 system、RTSP、WebRTC、record、producer、AI、pipeline、model 路由分组函数。
  - 将原 lambda 业务逻辑搬到命名 handler。
  - 保持 HTTP path、响应 message 和 JSON 字段兼容。
- 验证：
  - `cmake --build stream-sever/build/Debug` 成功。

## Step 7：双目拼接流默认适配
- 状态：已完成
- 完成时间：2026-07-12
- 修改文件：
  - `stream-sever/src/media_producer/simple_ipc/simple_ipc_config.h`
  - `stream-sever/src/media_producer/simple_ipc/simple_ipc_producer.h`
  - `stream-sever/src/media_producer/media_manager.h`
  - `stream-sever/src/main.cpp`
  - `stream-sever/src/http.cpp`
  - `memory-bank/*`
- 完成内容：
  - 新增 `R_STEREO_1280X480` 分辨率预设，对应 `1280x480 @ 30fps` 双目左右拼接帧。
  - SimpleIPC 默认分辨率从 `R_1080P` 切换为 `R_STEREO_1280X480`。
  - `main.cpp` 使用启动分辨率同步 WebRTC 视频参数和 MP4 录制宽高/fps。
  - `/api/pipeline/status` 的 `resolution.preset` 和 `available_resolutions` 增加 `stereo_1280x480`。
  - `/api/pipeline/resolution` 支持切换到 `stereo_1280x480`，原有 path、message 和 JSON 字段保持兼容。
- 验证：
  - `git diff --check` 成功。
  - `cmake --build stream-sever/build/Debug` 成功。
