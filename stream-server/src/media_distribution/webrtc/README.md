# WebRTC 模块

`media_distribution/webrtc` 使用 libdatachannel 把 H.264 送入浏览器 PeerConnection，并同时支持 WebSocket 信令客户端和 HTTP offer/answer/ICE 接口。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| [`webrtc_service.h/.cpp`](./webrtc_service.h) | `WebRTCService`：组合 signaling 与 WebRTC system，供 `StreamManager` 和 HTTP 使用。 |
| [`webrtc.h/.cpp`](./webrtc.h) | `WebRTCSystem`：PeerConnection、video track、H.264 RTP packetizer、SDP/ICE 和统计。 |
| [`signaling.h/.cpp`](./signaling.h) | `SignalingClient`：连接 signaling server、注册设备、加入房间、消息和自动重连。 |

## 媒体数据流

```text
EncodedStreamPtr
  -> WebRTCService::SendVideoFrame
  -> WebRTCSystem::SendVideoData
  -> H.264 RTP packetizer / media track
  -> PeerConnection
  -> browser
```

只有 WebRTC 已连接时才发送 frame。模块消费编码后的 H.264，不负责采集、MJPEG decode、像素转换或 VENC。

## 信令控制流

启动 service 时：

```text
create SignalingClient
  -> create/init WebRTCSystem
  -> connect signaling WebSocket
  -> on connected: JoinRoom
```

此外 HTTP API 可直接驱动：

- `POST /api/webrtc/offer`
- `POST /api/webrtc/answer`
- `POST /api/webrtc/ice`
- `GET /api/webrtc/candidates`

服务状态和生命周期由 `GET /api/webrtc/status`、`POST /api/webrtc/start|stop` 控制。

## 生命周期和所有权

- `StreamManager` 独占 `WebRTCService`。
- `WebRTCService` 以 `shared_ptr` 持有 `SignalingClient` 和 `WebRTCSystem`，确保异步回调期间对象存活。
- `Start()` 初始化 WebRTC 后连接信令；任一步失败返回 false，不伪造 running 状态。
- `Stop()` 先 Deinit WebRTC，再断开 signaling 并释放对象。
- `IsRunning()` 当前直接返回 `valid_`，表示初始化/启动成功，不等同于 PeerConnection 已连接；连接状态应使用 `IsConnected()`/`GetState()`。

## 执行上下文

H.264 consumer 注册为 `AsyncIO`，入口 callback 通常在全局 Asio context 执行；libdatachannel 的 WebSocket、PeerConnection 和 timer 还有自己的异步回调。跨回调捕获对象时必须维持现有 shared/weak ownership，避免 service Stop 后访问悬空对象。

## 配置

- `device_id` 默认由 `main.cpp` 设为 `aipc_camera`。
- `SIGNALING_HOST` 生成 `ws://<host>:8000/` 信令地址。
- `WebRTCConfig` 包含视频宽高、fps、bitrate、codec 和 ICE server 等参数。
- `main.cpp` 会按启动 producer 模式设置初始视频尺寸；后续 producer 冷切换分辨率不会自动重建 WebRTC config。

## 修改边界

- SDP、ICE、RTP/H.264 packetizer 逻辑属于 `WebRTCSystem`。
- signaling server 协议和重连属于 `SignalingClient`。
- HTTP envelope、status code 和 JSON 属于上层 `HttpApi`。
- 不在本模块反向访问 `MediaManager` 或选择摄像头模式。
