# USB UVC Producer

`uvc/` 负责 USB UVC 双目相机的设备发现、V4L2 MJPEG 采集、硬件解码/转换/编码以及独立诊断工具。

## 正式数据流

```text
/dev/videoX (MJPEG side-by-side)
  -> UvcProducer / V4L2 mmap
  -> owned UvcFramePtr
  -> latest-frame mailbox, capacity 1
  -> MjpegVdecDecoder: MJPEG -> YUV422P
  -> RgaNv12Converter: YUV422P -> NV12
  -> RKMPI VENC: NV12 -> H.264
  -> EncodedStreamDispatcher
```

相机的一帧包含左右目水平拼接画面。底层 capture 不拆左右目。

## 分层和文件职责

### Capture 层

| 文件 | 职责 |
| --- | --- |
| [`uvc_config.h`](./uvc_config.h) | UVC 三档双目 preset、目标格式、fps、buffer 数和 poll timeout。 |
| [`uvc_producer.h/.cpp`](./uvc_producer.h) | capability/format/size/fps 校验、设备发现、mmap、DQBUF/QBUF 和 owned frame callback。 |

`UvcProducer` 只输出拥有独立 `std::vector<uint8_t>` 的 MJPEG `UvcFramePtr`，不依赖 RKMPI VDEC/VENC、HTTP 或 distribution。

### 硬件处理层

| 文件 | 职责 |
| --- | --- |
| [`uvc_hardware_pipeline.h/.cpp`](./uvc_hardware_pipeline.h) | VDEC runtime、decoded frame RAII、RGA NV12 pool 和 move-only frame 所有权。 |
| [`uvc_h264_config.h`](./uvc_h264_config.h) | capture 配置、码率、GOP、VENC channel 和 input buffer 数。 |
| [`uvc_h264_producer.h/.cpp`](./uvc_h264_producer.h) | 组合 capture、容量 1 mailbox、VDEC、RGA、VENC 和 dispatcher。 |

正式进程仍以 compact `librockit.so` 为公共运行时；MJPEG VDEC 所需的 `librockit_full.so` 由 UVC 硬件组件通过 `dlopen(RTLD_LOCAL)` 按需加载，避免影响 SimpleIPC 的 RKAIQ 初始化。

## 设备发现和 buffer 所有权

自动发现遍历 `/dev/video*`，最终按 `uvcvideo` driver、Video Capture、Streaming、MJPEG、尺寸和 frame interval 全部校验。显式 `AIPC_UVC_DEVICE` 也走同一套校验，不能绕过能力检查。

```text
VIDIOC_DQBUF
  -> copy mmap bytes into owned UvcFrame
  -> VIDIOC_QBUF
  -> invoke callback with owned frame
```

mmap 指针只在 DQBUF/QBUF 临界区有效。耗时 decode 只能使用复制后的 owned frame，并且必须先 QBUF 再调用 callback。

## Latest-frame mailbox

capture thread 只替换一个 pending frame。处理线程忙时，新帧到达会丢弃旧 pending frame并保留最新帧，避免形成无上限队列和历史延迟。处理线程串行访问 VDEC/RGA/VENC 状态。

## 硬件 frame 生命周期

```text
UvcFramePtr retained by external VDEC input MB
  -> VDEC returns move-only VdecFrame
  -> synchronous RGA conversion
  -> release VdecFrame
  -> move-only Nv12Frame sent to VENC
  -> release caller MB reference after SendFrame returns
```

VDEC 实测输出为 YUV422P，即使初始化时请求 NV12也必须按每帧真实格式检查。RGA 输出 buffer 的 size、stride、virtual width/height 来自 CAL API；高分辨率下 virtual height 可能大于 visible height。

## 启停顺序

启动：dispatcher → processing thread → V4L2 capture。停止：拒绝新 frame → 停止 capture → 清 mailbox并 join processing → 停止 dispatcher。反初始化再按 VENC → RGA/VDEC → SYS → mmap/fd 的逆序释放。

## 分辨率和性能

三档 preset 为 `3840x1080`、`2560x720`、`1280x480`，均是左右目拼接尺寸。冷切换由 `MediaManager` 完整重建 capture/VDEC/RGA/VENC；不能只修改 `UvcConfig` 字段。

配置请求 30 fps 不表示整条链路一定达到 30 fps，尤其高分辨率必须查看 capture、hardware input 和 dispatcher output 的实测统计。

## 禁止事项

- 不让原始 MJPEG 冒充 `EncodedStreamPtr`。
- 不固定 `/dev/video0` 或只按设备名称选择。
- 不在 capture 层依赖 VDEC/VENC、HTTP 或 distribution。
- 不用无界队列积压旧帧。
- 不写死 NV12 buffer size/stride，不忽略实际 VDEC pixel format。
- 不加入静默 software fallback；硬件初始化/处理失败应明确返回和记录。
