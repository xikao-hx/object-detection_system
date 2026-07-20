# Common 公共基础设施

`common/` 提供被 producer、distribution、HTTP 等模块共同使用的 header-only 能力。它只依赖稳定的底层库，不承担业务状态或 service 所有权。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| [`asio_context.h`](./asio_context.h) | 全局 standalone Asio `io_context`、任务投递、停止、排空和重启。 |
| [`logger.h`](./logger.h) | 基于 spdlog 的按模块 logger 和 `LOG_*` 宏。 |
| [`media_buffer.h`](./media_buffer.h) | RKMPI frame/stream 的 RAII 包装、VENC 数据安全复制和地址/长度 helper。 |
| [`h264_nal_parser.h`](./h264_nal_parser.h) | H.264 Annex-B NAL 遍历、SPS/PPS 提取和关键帧判断。 |

## `IoContext`

`IoContext::Instance()` 是进程内统一的 Asio 事件循环。producer 的 `AsyncIO` consumer 从 VENC fetch thread 通过 `PostToIo()` 投递到这里，主线程在 `main.cpp` 调用 `Run()`。

关闭时先停止 producer，确保不再产生新任务，再调用 `Drain()` 释放队列中仍持有 `EncodedStreamPtr` 的任务，之后才释放 RKMPI 资源。

## 媒体资源所有权

`media_buffer.h` 中最重要的边界是 `acquire_encoded_stream()`：

```text
VENC GetStream
  -> copy valid pack bytes
  -> VENC ReleaseStream (same fetch thread)
  -> wrap copied bytes in external MB
  -> shared EncodedStreamPtr for consumers
```

因此当前 `EncodedStreamPtr` 指向可跨线程共享的副本，而不是延迟释放的原生 VENC pack。consumer 只读取数据，不应修改 pack、手动释放 MB 或调用 `RK_MPI_VENC_ReleaseStream`。

`VideoFramePtr` 则按来源在最后一个引用释放时调用对应 VI/VPSS release API。

## H.264 解析

`h264_nal_parser.h` 只处理 Annex-B start code 格式，返回的 `NalUnit` 指针引用调用方传入的 buffer，不拥有数据。RTSP、WebSocket Preview、MP4 等需要识别 SPS/PPS 或 IDR 时应复用这里的函数。

## 依赖规则

- `common` 可以依赖 Asio、spdlog 和 RKMPI 基础头文件。
- `common` 不依赖 `MediaManager`、`StreamManager`、HTTP 或任何具体 service。
- 只把至少两个模块稳定复用的逻辑放入这里；单一硬件链路 helper 留在其模块内部。
