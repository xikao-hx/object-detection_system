# Media Producer 架构

`media_producer/` 负责获得视频、完成必要的像素处理和编码，并统一输出 H.264 `EncodedStreamPtr`。它不负责 RTSP、WebRTC、WebSocket、MP4 或 HTTP。

## 组件

| 文件或目录 | 职责 |
| --- | --- |
| [`i_media_producer.h`](./i_media_producer.h) | producer 统一接口、通用配置、consumer 类型和工厂声明。 |
| [`media_manager.h/.cpp`](./media_manager.h) | producer 唯一持有者；生命周期、模式/分辨率冷切换和 consumer 注册信息保存。 |
| [`encoded_stream_dispatcher.h`](./encoded_stream_dispatcher.h) | VENC 取流线程、跨线程安全副本、低频统计和 consumer 分派。 |
| [`rk_venc_config.h`](./rk_venc_config.h) | SimpleIPC/UVC 共用的 VENC channel 配置 helper。 |
| [`simple_ipc/`](./simple_ipc/README.md) | 板载 MIPI/RKISP 的 VI → VPSS → VENC 硬件链路。 |
| [`uvc/`](./uvc/README.md) | USB UVC MJPEG 采集及 VDEC → RGA → VENC 链路。 |

## 统一接口

`IMediaProducer` 的稳定语义：

- `Init()` 分配硬件/软件资源，但不开始向 consumer 出流。
- `RegisterStreamConsumer()` 只能在运行前调用。
- `Start()` 开始采集、编码和 dispatcher 取流。
- `Stop()` 停止运行但保留可再次启动的资源。
- `Deinit()` 释放资源。
- `GetConfig()` 只返回 producer 共有参数；模式专属分辨率留在具体配置层。

输出始终是 H.264 `EncodedStreamPtr`。MJPEG、NV12、YUV422P 等中间数据不能进入此公共 consumer 接口。

## `MediaManager`

`MediaManager` 是单例策略上下文，持有唯一的 `std::unique_ptr<IMediaProducer>`。它还保存 consumer 注册信息，使 producer 被重建后能恢复相同的分发连接。

```text
Init(mode, config)
  -> CreateProducer(mode)
  -> producer.Init()
  -> register saved consumers

Start() -> producer.Start()
Stop()  -> producer.Stop()
Deinit()-> producer.Deinit() -> destroy producer
```

模式切换和分辨率切换均为冷切换：

```text
remember running state
  -> stop/deinit/destroy old producer
  -> create/init target producer
  -> re-register consumers
  -> restart if previously running
```

UVC 分辨率切换失败时会尝试用旧 preset 重建旧 producer。调用方不能把请求值直接当作已生效值，应读取 manager 或 pipeline status。

## Dispatcher 与 consumer

`EncodedStreamDispatcher` 拥有一个 VENC fetch thread。它通过 `acquire_encoded_stream()` 在同一线程完成原生 `GetStream/ReleaseStream`，然后把独立副本交给 consumer。

| 类型 | 当前执行位置 |
| --- | --- |
| `AsyncIO` | `asio::post` 到全局 `IoContext`。 |
| `Direct` | VENC fetch thread 直接调用。 |
| `Queued` | 当前也在 VENC fetch thread 直接调用。 |

当前 `queue_size` 参数只保存在注册信息中，dispatcher 尚未建立独立 consumer queue。不要假设 `Queued` 已隔离慢 I/O；在完善该能力前，慢 callback 会阻塞所有 VENC 取流和后续 consumer。

## 生命周期约束

- consumer 注册完成后才能启动 producer。
- 停止时先让 capture/processing 不再产生输入，再停止 dispatcher。
- 清 consumer 必须在 dispatcher 停止后执行。
- producer `Deinit()` 前需要排空仍持有 H.264 副本的异步 task。
- 新 producer 不得反向依赖 `MediaManager`、HTTP、`StreamManager` 或 distribution service。

## 新增 producer 的检查清单

1. 中间格式保持类型化，最终才转为 H.264 `EncodedStreamPtr`。
2. 所有资源有明确的 Init/Start/Stop/Deinit 逆序清理。
3. consumer 在 start 前注册，运行中拒绝修改。
4. VENC 配置优先复用 `rk_venc_config.h`。
5. 通过 `MediaManager` 创建和冷切换，不新增第二个全局 producer 入口。
6. distribution 和 HTTP 无需感知新 producer 的内部格式。
