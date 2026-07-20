# RTSP 模块

`media_distribution/rtsp` 把 producer 输出的编码流交给 Luckfox/Rockchip `rtsp_demo` 库。默认地址为 `rtsp://<device_ip>:554/live/0`。

## 分层

| 文件 | 职责 |
| --- | --- |
| [`rtsp_service.h/.cpp`](./rtsp_service.h) | `RtspService`：供 `StreamManager` 持有的启停门面，只在 running 时消费 frame。 |
| [`rk_rtsp.h/.cpp`](./rk_rtsp.h) | `RtspServer`：`rtsp_demo` handle/session、codec 配置、时间戳同步、发送和统计。 |

## 数据流

```text
EncodedStreamPtr
  -> RtspService::OnEncodedStream
  -> rtsp_stream_consumer
  -> RtspServer::SendVideoFrame
  -> rtsp_tx_video + rtsp_do_event
  -> RTSP client
```

`RtspService` 在构造时初始化底层全局 `RtspServer`，`Start()` 只把 service 切换为允许推帧；`Stop()` 停止消费但不销毁 server。析构时才反初始化底层 handle/session。

## 所有权和执行上下文

- `StreamManager` 独占 `RtspService`。
- 底层 `GetRtspServer()` 是模块内全局实例，不应在 `StreamManager` 外再创建并行 RTSP service。
- RTSP consumer 注册为 `AsyncIO`，通常在全局 Asio `IoContext` 执行。
- 输入是安全的 H.264/H.265 pack 副本；RTSP 模块只读取，不释放或修改 MB。

## HTTP 和配置

- `GET /api/rtsp/status`
- `POST /api/rtsp/start`
- `POST /api/rtsp/stop`

`RtspConfig` 定义端口、path 和 codec type。`main.cpp` 默认创建 RTSP service 但不自动开始消费，除非命令行带 `--rtsp` 或通过 HTTP 启动。

## 修改注意事项

- `Start()`/`Stop()` 是推帧开关，不要误认为每次都重建监听 socket。
- 统计来自 `RtspServer::Stats`，包含发送帧、字节和错误数。
- 新 codec 或时间戳策略应在 `RtspServer` 适配，HTTP 状态和 service 生命周期留在 `RtspService`。
