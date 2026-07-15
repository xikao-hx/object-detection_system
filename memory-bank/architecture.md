# 架构记录

## 目录/文件作用

- `stream-server/src/main.cpp`：进程配置、服务启动和 shutdown 编排。
- `stream-server/src/media_producer/i_media_producer.h`：现有 H.264/RKMPI producer 的统一接口，输出 `EncodedStreamPtr`。
- `stream-server/src/media_producer/media_manager.*`：现有 producer 生命周期和 H.264 consumer 注册中心。
- `stream-server/src/media_producer/simple_ipc/`：MIPI/RKISP 的 VI -> VPSS -> VENC 链路。
- `stream-server/src/media_distribution/stream_manager.*`：distribution service 的唯一所有权边界。
- `stream-server/src/media_producer/uvc/`：USB UVC 设备发现、V4L2 mmap 和 MJPEG 帧生产边界。
- `stream-server/src/media_producer/uvc/uvc_h264_producer.*`：MJPEG decode、NV12 conversion、RKMPI VENC 和 H.264 producer 生命周期。
- `stream-server/src/media_producer/uvc/uvc_vdec_probe.cpp`：独立验证真实 UVC MJPEG 的 RKMPI VDEC 兼容性和性能，不属于正式 producer 数据流。
- `stream-server/src/media_producer/encoded_stream_dispatcher.h`：SimpleIPC/UVC 共用的 VENC fetch 与 typed consumer 分发。
- `stream-server/src/media_producer/rk_venc_config.h`：两种 producer 共用的 H.264 VENC channel 配置。

## 当前约束

- 现有 `IMediaProducer`/`MediaManager` 输出语义是 H.264 `VENC_STREAM_S`，UVC 原始 MJPEG 不进入该接口。
- `StreamManager` 是 RTSP/WebRTC/WebSocket/File service 的唯一所有权边界。
- 运行时 consumer/callback 必须在 producer start 前注册；运行中禁止新增或清理。
- UVC 层只依赖 Linux/V4L2、C++ 标准库和 common 日志，不反向依赖 HTTP 或 distribution。
- UVC H.264 组合层可依赖 FFmpeg/RKMPI，但仍不得依赖 RTSP、HTTP 或 `StreamManager`。
- 共享 H.264 Annex-B 解析仍统一复用 `common/h264_nal_parser.h`；UVC MJPEG 不涉及该 parser。
- 性能观测归属数据所在边界：V4L2/owned copy 由 `UvcProducer` 统计，decode/NV12/VENC input 由 `UvcH264Producer` 统计，H.264 output 由共用 dispatcher 统计；不通过 HTTP 或 distribution 反向查询 producer 内部状态。

## 数据流

```text
当前 SimpleIPC：VI -> VPSS -> VENC H.264 -> MediaManager -> StreamManager consumers
Step 1 UVC： /dev/videoX -> V4L2 mmap -> owned MJPEG UvcFrame -> typed callback/test tool
Step 3 UVC： UvcFrame -> MJPEG decode -> NV12 -> RKMPI VENC H.264 -> existing consumers
Step 5 UVC： capture thread -> latest-frame mailbox(1) -> processing thread -> decode/NV12/VENC -> existing consumers
Step 6 probe： /dev/videoX -> UvcProducer owned MJPEG -> RKMPI VDEC -> validate/save actual NV12 or YUV422P
Step 7 probe： /dev/videoX -> UvcProducer owned MJPEG -> RKMPI VDEC YUV422P -> RGA -> NV12 MB -> validate/save
```

## 新增洞察

- 双目相机的“左右目”是单个 `1280x480` JPEG 的图像语义，不应在底层 V4L2 producer 中拆成两个媒体生命周期。
- 自动发现的判断基准是 V4L2 driver/capability/format，而不是不稳定的节点号或仅凭设备显示名。
- `uvc_lib` 是可独立构建和验收的 capture library；调用方包括板端 `uvc_capture_test` 和组合层 `UvcH264Producer`，原始 MJPEG 仍不直接进入 distribution 生命周期。
- 用户取消独立 MJPEG/PyQt transport；UVC 必须在 producer 层转换为 H.264 后才能进入现有 RTSP、WebRTC、WebSocket Preview 和 File consumers。
- `aipc` 是统一链接各 distribution 模块的单一二进制；动态加载器会在进入 `main()` 前解析 `DT_NEEDED`，所以部署包必须包含板端系统目录未提供的直接依赖，不能按本次启用的传输参数裁剪 `libdatachannel`。
- Step 3 板端验收已通过，确认 distribution 层无需感知 H.264 来自 SimpleIPC 还是 UVC producer。
- Step 4 只增加模块私有的累计计时和低频日志，当前同步数据流与所有权未变化；未来 latest-frame queue 只能由 `UvcH264Producer` 拥有，不得下沉到通用 `UvcProducer` 或上浮到 distribution。
- Step 5 的容量 1 mailbox 和 processing thread 均由 `UvcH264Producer` 私有拥有；`UvcProducer` 仍只提供 owned MJPEG callback，dispatcher 仍只消费 H.264。过载时替换 pending 旧帧，不允许跨层积压或 drain 历史帧。
- Step 6 的 VDEC 是独立诊断 target，固定独占 channel 0 并要求正式服务停止后运行；它不改变 `MediaManager`、正式 producer 或 distribution 的依赖和所有权。该步骤已完成板端验收，正式替换前仍需独立证明 YUV422P -> NV12 转换后的端到端帧预算。
- VDEC decoded frame 的 pixel format 必须以每帧 `VIDEO_FRAME_INFO_S` 为准；请求 NV12 不代表 JPEG hardware decoder 必然执行 chroma resampling。该相机实测返回 YUV422P，后续格式转换仍应位于 producer 内部硬件处理边界，不能下沉到通用 UVC capture 或上浮到 distribution。
- Step 7 仍是同一独立诊断 target 的显式模式：RGA 只通过 decoded/output MB 的 DMA fd 工作，NV12 pool 由 probe 私有拥有；它不改变正式 producer 或 distribution 的所有权。若验证通过，后续正式设计可在 producer 内部复用该硬件边界，但不能直接把 probe 生命周期变成公共 service。
- Step 7 已通过三次板端 300 帧、约 30fps、零错误和 NV12 画面验收，证明 `VDEC YUV422P -> RGA NV12` 是正式 producer 的可行局部替换基础；下一步仍需单独设计 VDEC/RGA/VENC 生命周期、latest-frame 过载策略和失败传播。
