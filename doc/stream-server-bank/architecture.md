# 架构记录

## 当前所有权
- `StreamManager` 使用 `std::unique_ptr` 持有 distribution services。
- `MediaManager` 持有当前活动的 `IMediaProducer` 和已保存的 stream consumer 注册信息。
- `SimpleIPCProducer` 持有 RKMPI 管线生命周期和内部 stream dispatcher。

## 结构约束
- distribution services 应通过 `StreamManager` 访问，不应再使用独立的全局 service 单例。
- producer/common 层不能依赖 distribution services。
- HTTP handler 可以调用 manager/service 的 public interface，但不能检查内部状态。
- 被多个 distribution 模块共享的 H.264 解析逻辑应放在 `common/`。

## Step 1 约束
- VENC 配置流向为 `ProducerConfig` -> `SimpleIPCConfig` -> `venc_init()`。
- stream shutdown 必须停止 `StreamManager` 持有的每一个已启动 service。
- `MediaManager` 的 const 方法应直接使用已有 mutable mutex。

## Step 1 结果
- `StreamManager::Stop()` 负责停止正在录制的文件、RTSP、WebRTC 和 WebSocket 预览。
- `mpi_config.h::venc_init()` 保持为基础 RKMPI helper，并接收标量 encoder 参数。
- `SimpleIPCProducer` 仍然是把 `SimpleIPCConfig` 适配到 RKMPI helper 调用的层。

## Step 2-3 结果
- distribution service 不再暴露独立全局单例 helper。
- `StreamManager` 是 RTSP、File、WebRTC、WebSocket Preview service 的唯一所有者。
- service consumer 回调统一为类型化成员函数：
  - `RtspService::OnEncodedStream`
  - `FileService::OnEncodedStream`
  - `WsPreviewServer::OnEncodedStream`
  - `WebRTCService::SendVideoFrame`
- consumer 注册只能发生在 producer/dispatcher 启动前；运行中注册或清理会被拒绝并记录 warning。

## Step 4 结果
- `common/h264_nal_parser.h` 提供 H.264 Annex-B NAL 遍历、SPS/PPS 查找和关键帧判断。
- common parser 不依赖 distribution service、HTTP 或 manager 层。
- MP4、WebSocket Preview、WebRTC 共享同一套 NAL 解析规则。

## Step 5 结果
- `Mp4Recorder::WriteFrame()` 写完当前帧后检查录制时长限制。
- 达到 `maxDurationSec` 后在锁外调用 `StopRecording()`，保证 trailer/close 路径可以正常获取锁。

## Step 6 结果
- `HttpApi::SetupRoutes()` 只负责调用路由分组注册函数。
- 具体业务逻辑位于 `HandleXxx` 成员函数。
- HTTP API 兼容性约束保持不变：路径、message 和 JSON 字段不主动改名。

## Step 7 结果
- SimpleIPC 新增 `R_STEREO_1280X480` 预设，表示 `1280x480 @ 30fps` 双目左右拼接帧。
- `MediaManager` 默认 SimpleIPC 分辨率为双目拼接预设，板端仍保持单 producer、单 VENC、单 H.264 输出链路。
- `main.cpp` 将 WebRTC 和 MP4 录制的默认宽高/fps 与启动分辨率保持一致。
- HTTP 分辨率接口仅扩展 `stereo_1280x480` preset 值，旧 path、message 和 JSON 字段继续兼容。
- 双目拆分、校正、深度和测距算法仍由 PyQt/客户端处理，不进入板端 RKMPI 推流链路。
