# WebSocket Preview 模块

`wspreview/` 通过 libdatachannel WebSocket server 把 H.264 Annex-B 数据直接发送给浏览器。Web 前端使用 JMuxer/MSE 播放，默认端口为 `8082`。

## 文件和类

| 文件 | 职责 |
| --- | --- |
| [`ws_preview.h`](./ws_preview.h) | `WsPreviewConfig`、`WsPreviewServer` 接口和客户端/SPS/PPS 状态。 |
| [`ws_preview.cpp`](./ws_preview.cpp) | server 启停、客户端生命周期、广播和 H.264 参数集缓存。 |

## 数据流

```text
EncodedStreamPtr
  -> WsPreviewServer::OnEncodedStream
  -> read copied H.264 pack
  -> parse/cache SPS and PPS
  -> WebSocket binary send to each open client
```

每个 VENC 输出 pack 通常对应一条 WebSocket binary message。模块不做 H.264 解码、转码或容器封装。

## 新客户端接入

服务端持续从 Annex-B 数据中提取并缓存最近的 SPS/PPS。新连接在 WebSocket `onOpen` 后先收到缓存的 SPS 和 PPS，再等待后续视频 pack。若连接发生在 GOP 中间且尚无缓存，客户端需等待下一次包含参数集/关键帧的数据。

客户端由 `shared_ptr` 列表持有；关闭回调从列表移除连接。`Stop()` 先交换出客户端列表，在锁外调用 `close()`，避免同步关闭回调再次获取同一 mutex 导致死锁。

## 线程和锁

- `OnEncodedStream` 作为 `AsyncIO` consumer 通常在全局 `IoContext` 执行。
- libdatachannel 自己管理 WebSocket 回调线程。
- `clients_mutex_` 只保护客户端容器；发送前复制 `shared_ptr` 列表，避免持锁进行网络操作。
- `sps_pps_mutex_` 保护参数集缓存。

## 配置与限制

- 默认监听 `0.0.0.0:8082`，无 TLS。
- 默认最多 5 个客户端。
- 启动端口失败最多重试 3 次，每次间隔 1 秒。
- `keyframe_interval_ms` 当前存在于配置结构中，但发送路径没有用它主动请求或节流关键帧。
- 当前没有独立 HTTP 启停 endpoint；服务由 `StreamManager` 随进程启停。

H.264 NAL 解析统一使用 `common/h264_nal_parser.h`，不要在本目录复制 start-code 扫描逻辑。
