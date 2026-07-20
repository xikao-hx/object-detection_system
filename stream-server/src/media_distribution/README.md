# Media Distribution 架构

`media_distribution/` 负责把 producer 输出的同一份 H.264 `EncodedStreamPtr` 交给不同协议或存储服务。它不采集图像、不解码 MJPEG，也不控制 producer 硬件。

## 所有权

`StreamManager` 是所有 distribution service 的唯一所有权边界：

```text
StreamManager
  |-- unique_ptr<RtspService>
  |-- unique_ptr<WebRTCService>
  |-- unique_ptr<WsPreviewServer>
  `-- unique_ptr<FileService>
```

`CreateStreamManager()`、`GetStreamManager()` 和 `DestroyStreamManager()` 提供进程级访问入口。不要在其他位置再创建 service 单例或保存第二份所有权。

## 子模块

| 目录 | 输入和输出 | 控制入口 |
| --- | --- | --- |
| [`rtsp/`](./rtsp/README.md) | H.264/H.265 → Rockchip RTSP server | `/api/rtsp/*` |
| [`webrtc/`](./webrtc/README.md) | H.264 → libdatachannel PeerConnection | `/api/webrtc/*` |
| [`wspreview/`](./wspreview/README.md) | H.264 Annex-B → WebSocket binary message | `StreamManager` 启停 |
| [`file/`](./file/README.md) | H.264/H.265 → FFmpeg MP4 muxer | `/api/record/*` |

## Consumer 连接

service 不主动读取 producer。`main.cpp` 在 producer 启动前把 service 方法注册到 `MediaManager`：

```text
EncodedStreamDispatcher
  |-- "rtsp"      -> RtspService::OnEncodedStream
  |-- "ws_preview"-> WsPreviewServer::OnEncodedStream
  |-- "file"      -> FileService::OnEncodedStream
  `-- "webrtc"   -> WebRTCService::SendVideoFrame
```

模式或分辨率冷切换时，`MediaManager` 用保存的注册信息把同一组 callback 重新挂到新 producer，distribution service 无需重建。

## 生命周期

`StreamManager` 构造函数根据 `StreamConfig` 创建 service。`Start()`：

- 仅在 `auto_start_rtsp` 为 true 时启动 RTSP 消费。
- 仅在 `auto_start_webrtc` 为 true 时初始化 WebRTC/信令。
- 已创建的 WebSocket Preview 会直接启动。
- `FileService` 当前不在 `StreamManager::Start()` 中单独启动；录制由 HTTP 调用 recorder 控制。

`Stop()` 依次停止录制、RTSP、WebRTC 和 WebSocket Preview；析构再次调用 `Stop()`，service 自身也用 RAII 清理底层对象。

## 数据和配置边界

- distribution 只读 `EncodedStreamPtr`，不能修改或手动释放其中的 MB。
- SPS/PPS/IDR 识别必须复用 `common/h264_nal_parser.h`。
- `StreamConfig` 在 `main.cpp` 组装，包含各 service 的配置和 auto-start 开关。
- 当前 WebRTC/MP4 的宽高/fps 是 service 初始化配置；producer 冷切换分辨率时不会自动重建全部 distribution 配置，扩展动态分辨率前需单独设计。

## 当前并发事实

网络 consumer 使用 `AsyncIO`，callback 被投递到全局 Asio context。File consumer 虽注册为 `Queued`，但当前 dispatcher 未实现独立队列，实际仍在 VENC fetch thread 直接执行 `FileService::OnEncodedStream`。增加耗时写盘逻辑前必须先处理该边界，不能依赖枚举名称推断线程隔离。
