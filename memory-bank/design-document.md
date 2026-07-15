# 双目 UVC 图像采集设计文档

## 需求理解

- 目标设备是 Luckfox Pico Ultra W，输入设备是 USB 2.0 UVC 双目相机 HBVCAM-4M2214HD-2 V11。
- 相机推荐输出 `MJPG 1280x480 30fps`，单帧是左目 `640x480` 与右目 `640x480` 的水平拼接。
- `/dev/videoX` 编号不稳定，必须按驱动、能力和格式运行时发现。
- 现有 distribution consumer 只接受 H.264 `EncodedStreamPtr`，因此 MJPEG 必须在 producer 内完成解码和重新编码。

## 需求澄清

### 已确认事项

- Step 1 已完成直接 MJPEG 采集验证；用户随后取消路线 A 的 PyQt/MJPEG transport，要求直接接入 RTSP。
- V4L2 IO 使用 mmap，默认申请 4 个 buffer。
- 板端只负责完整拼接帧采集，不做左右目拆分。

### 已知性能边界

- 板端纯 V4L2 采集 300 帧耗时 10.32 秒，约 `29.1fps`，UVC 输入能力正常。
- 首版 FFmpeg software MJPEG decode + swscale + VENC 完整链路实测约 `14fps`；该性能不阻塞 Step 3 功能验收，后续若要求稳定 30fps，应局部替换为硬件 JPEG decode。
- Step 4 的 500 帧分段统计为：capture/input/output 均约 `13.53fps`，sequence gap 为 603；decode 平均 `24.51ms`、swscale 平均 `48.26ms`、总处理平均 `73.61ms`，两段软件处理合计约占总耗时 `98.9%`。
- MJPEG decoder 输出 legacy YUVJ full-range pixel format 时，swscale 必须改用同布局非 J format，并显式配置 JPEG full-range -> limited-range NV12；禁止通过降低 FFmpeg 日志级别隐藏 range warning。

### 性能优化策略

- 先保持同步数据流不变，在 V4L2 capture、FFmpeg decode、swscale、RKMPI input 和 VENC output 边界增加每 100 帧一次的汇总统计。
- 统计确认后再用容量 1~2 的 latest-frame queue 解耦采集与转码；队列满时丢旧帧，禁止积累历史延迟。
- RKMPI MJPEG VDEC 必须先由独立板端 probe 验证，再替换正式 producer；无实机证据时不提前实现 zero-copy 或 VDEC -> VENC bind。
- 首次真实 probe 已证明 VDEC 能解码该相机 MJPEG，但尽管请求 NV12，实际返回 `RK_FMT_YUV422P`（format 7、`1280x480`、MB 1228800 bytes）；probe 必须保留并报告真实格式，后续是否用 RGA/VPSS 转 NV12 需另立步骤验证。
- Step 7 优先验证现有链接依赖 librga：以 decoded YUV422P MB 和独立 NV12 MB 的 DMA fd 做同步 `imcvtcolor`，分别测量转换与端到端预算。该 probe 通过前不修改正式 producer；失败时保留原始 RGA 错误，不自动退回软件转换或同时引入 VPSS。

## 功能设计

### UVC 设备发现

- 输入：目标 pixel format、宽、高、fps，以及可选的显式设备路径。
- 处理：遍历 `/dev/video*`，通过 `VIDIOC_QUERYCAP` 检查 `uvcvideo`、Video Capture、Streaming，再枚举 format/frame size/frame interval。
- 输出：首个完全满足目标规格的设备路径。
- 异常：没有合格节点时返回空结果并记录错误，不回退到 rkisp/rkcif。

### V4L2 mmap 采集

- 输入：`UvcConfig` 与 MJPEG 帧回调。
- 处理：open、format/fps 配置、request/query/mmap/queue buffers、stream on、poll/dequeue/copy/callback/requeue。
- 输出：拥有独立字节存储、sequence 和 capture timestamp 的 `UvcFrame`。
- 异常：系统调用失败时保留 errno 上下文；停止时唤醒采集循环并完整清理。

### 板端诊断

- 输入：可选 device、采集帧数和首帧输出路径。
- 处理：运行同一个 `UvcProducer`，等待目标帧数或超时。
- 输出：首帧 JPEG 和采集统计；失败时返回非零状态。

### UVC H.264 producer

- 输入：owned MJPEG `UvcFrame`。
- 处理：FFmpeg MJPEG decode -> swscale NV12 -> RKMPI MB pool -> VENC H.264。
- 输出：与 SimpleIPC 相同的 `EncodedStreamPtr`，复用 RTSP/WebRTC/WS/File typed consumer。
- 异常：解码、转换、MB、SendFrame 失败均计数并记录；单帧失败不伪造输出。

## 关键流程

1. 构造 producer 并在启动前注册帧回调。
2. `Init()` 解析设备并建立 mmap 资源，但不启动流。
3. `Start()` 执行 `VIDIOC_STREAMON` 并创建采集线程。
4. 线程将每个有效 buffer 复制成独立 `UvcFrame` 后调用回调；UVC H.264 组合层的回调只替换容量 1 的 latest-frame mailbox。
5. `Stop()` 关闭 stream 并等待线程退出；`Deinit()` 解除 mmap 和关闭 fd。
6. UVC H.264 私有处理线程从 mailbox 取得最新 owned frame，执行 decode/NV12/VENC，并把编码输出交给共用 dispatcher。

## 数据与状态

- `UvcConfig`：设备选择和目标采集规格。
- `UvcFrame`：一帧 MJPEG bytes、宽高、sequence、timestamp。
- 状态顺序：constructed -> initialized -> running -> initialized -> deinitialized。
- 回调注册只允许在非 running 状态发生，避免运行期 consumer 竞争。

## 验收标准

- 交叉构建通过且无新增 warning/error。
- 显式设备与自动发现都执行同一套能力校验。
- 板端采集 100 帧并保存可解码的 `1280x480` JPEG。
- 现有 SimpleIPC、HTTP path、响应 message 和 JSON 字段不变。
- Step 3 已由用户确认板端验收通过。
- 性能统计能分别给出 V4L2 DQBUF fps、sequence gap、MJPEG copy、decode、swscale、MB 获取、cache flush、VENC send 和 H.264 output fps，且不以逐帧 INFO 日志干扰结果。
