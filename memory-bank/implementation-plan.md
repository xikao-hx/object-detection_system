# 双目 UVC 图像采集实施计划

## 总体原则

- 每一步保持可独立验证；用户确认板端测试通过前不进入下一步。
- 先建立 MJPEG 采集边界，再选择传输协议，最后才评估 H.264 转码。
- 不让 MJPEG 冒充现有 H.264 `EncodedStreamPtr`。

## Step 1：独立 UVC/V4L2 MJPEG 采集闭环

### 目标

- 自动发现合格的 UVC capture 节点。
- 完成 mmap 采集生命周期与类型化 MJPEG 帧回调。
- 提供板端诊断程序，采集指定帧数并保存首帧。

### 涉及文件/模块

- `stream-server/src/media_producer/uvc/`：UVC 配置、producer、诊断程序和 CMake target。
- `stream-server/src/media_producer/CMakeLists.txt`：只负责纳入 UVC 子模块。

### 指令

- 实现统一的 candidate capability 检查，显式设备和自动发现复用该逻辑。
- `Init()` 只分配资源，`Start()` 才开启 streaming/thread。
- 回调帧必须拥有自己的 MJPEG bytes，不能暴露即将重新 QBUF 的 mmap 地址。
- 所有失败路径按初始化逆序清理。

### 结构验收点

- UVC 不依赖 RKMPI 或 distribution 层。
- 没有修改 `IMediaProducer` 的 H.264 类型语义。
- 运行中禁止替换/清空回调。
- 未提前实现网络传输、JPEG decode 或 H.264 encode。

### 测试

#### 自动检查

```bash
git diff --check
cmake --build stream-server/build/Debug
```

#### 板端场景

```bash
./uvc_capture_test --frames 100 --output /tmp/uvc_first.jpg
ls -lh /tmp/uvc_first.jpg
```

#### 预期日志关键字

- `Selected UVC device`
- `UVC producer started`
- `Captured 100 frame(s)`

## Step 2：取消

- 2026-07-13 用户明确取消独立 MJPEG WebSocket/PyQt 阶段。
- 不保留该阶段的 service、客户端或协议代码。
- PyQt 推理暂不在当前范围内。

## Step 3：UVC H.264 与现有三种网络分发接入

### 进入条件

- 用户明确要求跳过 Step 2，先以 RTSP 验证完整推流链路，并保持现有 RTSP、WebRTC、WebSocket H.264 三种网络传输能力。

### 目标

- `MJPEG -> decode -> NV12 -> VENC H.264` 后才接入现有 typed consumer。
- `aipc --mode uvc` 自动发现 UVC 相机，并复用现有 RTSP、WebRTC、WebSocket H.264 consumers。
- `assets/start_app.sh` 保持原有默认启动三种网络输出的使用方式，并支持 `--mode uvc`。

### 涉及文件/模块

- `media_producer/encoded_stream_dispatcher.h`：SimpleIPC/UVC 共用的 H.264 stream fetch/consumer 分发。
- `media_producer/uvc/uvc_h264_producer.*`：MJPEG decode、NV12 conversion、VENC 输入和生命周期。
- `media_producer/i_media_producer.h`、`media_manager.*`：增加类型化 `UvcH264` producer mode。
- `main.cpp`：支持 `--mode uvc`，启动顺序和现有 consumer 注册不变。
- `http.cpp`：保持 endpoint/message/字段，扩展 mode 值和 available modes。

### 约束

- `StreamManager` 继续作为 distribution service 唯一所有权边界。
- consumer 在 producer start 前注册，运行中禁止新增或清理。
- UVC 层不依赖任一 distribution service；只输出与 SimpleIPC 相同的 `EncodedStreamPtr`。
- 首版用 FFmpeg MJPEG decoder + swscale 转 NV12，再送 RV1106 RKMPI VENC；板端日志必须暴露 decode/send 失败和实际帧率。
- 输入 MB 使用 RKMPI pool，`SendFrame` 后释放调用方引用，由 VENC 持有队列引用。

### 验收

```bash
cd /root/aipc/bin
AIPC_UVC_DEVICE=/dev/video0 ./start_app.sh --mode uvc
ffplay -fflags nobuffer -flags low_delay rtsp://<device-ip>:554/live/0
```

- 预期 producer 日志显示 UVC 节点、MJPEG decoder、VENC 和 H.264 fetch 均已启动。
- RTSP 客户端能持续看到 `1280x480` 左右拼接画面。
- WebSocket Preview 监听 `8082`，WebRTC 使用既有 WebUI/HTTP API 和 signaling 配置。

### 验收结果

- 2026-07-13 用户确认 Step 3 验收通过。
- UVC `1280x480` 拼接画面已进入既有 H.264 分发链路，RTSP、WebRTC 和 WebSocket Preview 保持原入口。
- 当前约 `14fps` 是已记录的软件 MJPEG 解码性能边界，不影响本步骤功能验收结论。

## Step 4：帧率优化阶段 1——分段性能统计

### 进入条件

- Step 3 已通过板端功能验收；纯 V4L2 采集约 `29.1fps`，完整 software decode + swscale + VENC 输出约 `14fps`。

### 目标和成功标准

- 保持当前同步数据流不变，以低频汇总日志量化 V4L2 DQBUF fps、sequence 跳号、owned MJPEG copy、FFmpeg decode、swscale、MB 获取、cache flush、VENC send 和 dispatcher output fps。
- 每 100 帧输出一次汇总，不新增逐帧 INFO 日志。
- 板端日志能判断帧丢在 V4L2/callback 边界还是 VENC output 边界，并能比较 decode、swscale 与 VENC input 各段耗时。

### 修改文件

- `media_producer/uvc/uvc_producer.cpp`：采集帧率、sequence gap 和 owned copy 统计。
- `media_producer/uvc/uvc_h264_producer.cpp`：decode、swscale、MB、flush、VENC send 分段统计。
- `media_producer/encoded_stream_dispatcher.h`：VENC H.264 实际输出帧率统计。
- `memory-bank/*.md`：记录本步设计、边界、验证和进度。

### 禁止修改范围

- 不增加处理线程或 latest-frame queue；该内容属于下一阶段。
- 不接入 RKMPI VDEC，不修改 FFmpeg decoder、像素格式、VENC 参数或 MB 所有权。
- 不修改 distribution、HTTP、Web UI、启动参数和现有 JSON 字段。

### 验证

```bash
git diff --check
cmake --build stream-server/build/Debug
```

板端运行 `start_app.sh --mode uvc`，至少观察 300 个输入/输出帧，并保存各段汇总日志。本步骤经用户确认板端统计有效后，才进入采集/转码解耦。

### 当前结果

- 2026-07-14 已完成代码和 memory-bank 更新，`git diff --check` 与 Debug 交叉构建通过。
- 板端已运行到 500 帧：capture/input/output 均约 `13.53fps`，sequence gap 603，decode/sws/total 平均耗时分别为 `24.51/48.26/73.61ms`，decoder 和 VENC send error 均为 0。
- 数据已证明 dispatcher 无额外丢帧，sequence gap 来自同步软件转码阻塞采集；decode + swscale 约占总耗时 `98.9%`，其中 swscale 是第一瓶颈。
- 验收日志同时暴露逐帧 `deprecated pixel format used, make sure you did set range correctly`。本步骤先补充显式 JPEG full-range -> limited-range NV12 配置并复测；warning 消失且画面色彩正常前不进入下一阶段。

### 验收结果

- 2026-07-14 用户提供修正后 300 帧日志，未再出现 deprecated pixel-format warning。
- capture/input/output 为 `13.84/13.81/13.81fps`，sequence gap 344；HTTP status、AI status 和 pipeline status 均返回 200。
- decode/sws/total 平均耗时为 `26.34/44.75/71.91ms`，再次确认 software decode + swscale 是瓶颈，dispatcher 没有额外丢帧。

## Step 5：帧率优化阶段 2——采集与转码解耦

### 目标和成功标准

- `UvcProducer` callback 只把 owned MJPEG 交给容量 1 的 latest-frame mailbox，不再同步执行 decode/sws/VENC。
- `UvcH264Producer` 私有处理线程消费 mailbox；新帧到达且已有待处理帧时丢弃旧帧并保留最新帧。
- V4L2 capture 恢复接近 30fps，sequence gap 显著下降；software H.264 output 允许仍约 14fps。
- 每 100 个成功送帧汇总 mailbox drop、最大深度和等待时间，证明过载不会形成无上限积压。

### 修改文件

- `media_producer/uvc/uvc_h264_producer.cpp`：mailbox、处理线程、统计和 shutdown 编排。
- `memory-bank/*.md`：记录本步设计、边界、验证和结果。

### 禁止修改范围

- 不修改通用 `UvcProducer`、`EncodedStreamDispatcher`、distribution、HTTP 或 Web UI。
- 不接入 RKMPI VDEC，不改变 FFmpeg decoder、swscale、VENC 参数和 MB 所有权。
- 不使用无上限队列，不为了提高输出计数处理过期帧。

### 验证

```bash
git diff --check
cmake --build stream-server/build/Debug
```

板端至少运行 500 个 capture 帧，核对 capture fps、sequence gap、mailbox drops、wait avg/max 和 output fps；同时验证启动、停止、重复启停及 RTSP/WebRTC/WebSocket Preview。

### 当前结果

- 容量 1 latest-frame mailbox、独立处理线程、主动丢旧帧统计和 shutdown 编排已实现。
- `git diff --check` 与 Debug 交叉构建通过，`aipc` 链接成功。
- 等待板端验收；用户确认前不进入 RKMPI MJPEG VDEC probe 阶段。

### 验收结果

- 2026-07-14 用户提供 500 个 capture 帧日志：capture `30.07fps`、sequence gap 0，mailbox depth `1/1`，wait avg/max `17.08/35.27ms`。
- 200 个 H.264 output 时 mailbox drops 256，output `13.16fps`；过载已转化为有界主动丢旧帧，没有采集丢帧或延迟积压。
- HTTP status、AI status、pipeline status 持续返回 200；Step 5 验收通过。

## Step 6：帧率优化阶段 3——RKMPI MJPEG VDEC 独立 probe

### 目标和成功标准

- 建立独立 `uvc_vdec_probe`，复用 `UvcProducer` 连续向 RKMPI VDEC 发送 300 个真实 UVC MJPEG frame。
- 验证 `VIDEO_MODE_FRAME + RK_VIDEO_ID_MJPEG` 能稳定输出 `1280x480`，准确记录实际 pixel format、virtual width/height、stride、MB size 和 send/get/total avg/max。
- 接受请求得到的 NV12，或硬件按 JPEG subsampling 返回的 `RK_FMT_YUV422P`；按实际格式保存首个 packed frame，供板端或 PC 人工确认左右方向、亮度和色彩，禁止把 YUV422P 命名或解析为 NV12。
- 任一 Create/Set/Start/Send/Get/Release、格式/尺寸或输出写入失败都返回非零，不吞错。

### 修改文件

- `media_producer/uvc/uvc_vdec_probe.cpp`：独立参数、VDEC 生命周期、真实 UVC 输入、统计和首帧输出。
- `media_producer/uvc/CMakeLists.txt`：新增 probe target，仅链接 `uvc_lib`、RKMPI VDEC 完整库及其直接依赖。
- `stream-server/CMakeLists.txt`：将 probe 和板端所需 `librockit_full.so` 加入安装产物。
- `memory-bank/*.md`：记录本步设计、边界、验证和结果。

### 禁止修改范围

- 不修改 `UvcH264Producer` 的正式 software decode 链路。
- 不接入 VENC、dispatcher、distribution、HTTP 或 Web UI。
- 不实现 VDEC -> VENC、MB 复用、zero-copy 或 channel bind。

### 验证

```bash
git diff --check
cmake --build stream-server/build/Debug
cmake --install stream-server/build/Debug
```

板端停止 `aipc` 后独占 VDEC channel 0 运行：

```bash
/root/aipc/bin/uvc_vdec_probe --device /dev/video0 --frames 300 --output /tmp/uvc_vdec_first.yuv422p
```

用户确认真实 MJPEG 连续解码、性能、按实际格式保存的画面和重复运行通过前，不替换正式 producer。

### 当前结果

- 独立 probe 已实现真实 UVC owned MJPEG 输入、external MB 生命周期、同步 Send/Get/Release、实际输出格式校验、packed NV12/YUV422P 首帧保存和每 100 帧统计。
- SDK 符号核验确认 VDEC API 由 `librockit_full.so` 导出；probe 单独链接 `rockit_full/rockchip_mpp/drm/rga`，正式 `aipc` 的精简 `rockit` 链接保持不变。
- 安装包已包含 `bin/uvc_vdec_probe` 和 `lib/librockit_full.so`，可通过 `$ORIGIN/../lib` 加载；Debug 全量交叉构建和 install 通过。
- 首次板端反馈运行的是正式 `aipc` 而非 probe，并在约 100 capture 帧后发生内核级 USB disconnect，因此不构成 Step 6 测试结果。
- USB 复查显示 descriptor read 持续 `error -110` 并最终无法枚举，`/dev/video0` 已消失；恢复真实 UVC 节点是继续 probe 验收的前置条件，禁止用 `rkisp/rkcif` 视频节点代替。
- 后续正式链路已稳定采集 600 帧、约 `30.06fps` 且 sequence gap 0，USB 枚举前置条件已恢复；该日志仍来自 `aipc`，只再次确认 Step 5，不计入 Step 6。
- 继续运行到 700 capture 帧后 USB 再次由内核断开并发生 descriptor timeout；在运行 probe 前，先停止 `aipc` 并以 `v4l2-ctl` 独立连续采集至少 1000 帧。若独立采集同样断开，优先处理供电、线材、接口或相机，不进入 VDEC 结论。
- 掉线后外层 H.264 producer/dispatcher 不会随 capture thread 自动停止，HTTP 仍可返回 200 且 VENC fetch 持续超时；这是独立的错误状态传播/恢复设计项，不属于 Step 6 decoder 能力验证范围。
- 首次真正运行 probe 已成功得到一帧无压缩 `1280x480` 输出；SDK 返回 format 7，即 `RK_FMT_YUV422P`，MB size `1228800 = 1280x480x2`。原 probe 因只接受 NV12 而主动失败，VDEC format/size/error status 均未报告硬件解码错误。
- 修正后已连续完成 300 帧：VDEC `29.84fps`，total avg/max `24.810/77.615ms`；输出 YUV422P `1280x480`、stride 1280、packed 1228800 bytes，错误计数全 0，生命周期正常释放。
- 后续两次重复运行也各完成 300 帧，最终 `29.96/29.85fps`、total avg `24.719/24.576ms`，错误计数全 0、无 USB disconnect、资源正常释放。

### 验收结果

- 用户确认 packed YUV422P 画面的左右拼接方向、亮度和色彩正常。
- 性能、三次连续运行、生命周期、格式/文件大小、错误状态和人工画面检查全部通过；Step 6 验收完成。
- 该结果只证明 `MJPEG -> VDEC YUV422P` 能接近 30fps，不证明 `YUV422P -> NV12 -> VENC` 已满足 30fps；正式 producer 保持 software decoder，后续转换方案必须另立步骤验证。

## Step 7：帧率优化阶段 4——RGA YUV422P 转 NV12 独立 probe

### 目标和成功标准

- 在现有 `uvc_vdec_probe` 中增加显式 `--convert-nv12` 模式，复用 Step 6 的真实 UVC MJPEG 与 RKMPI VDEC 生命周期。
- 当 VDEC 实际输出 `RK_FMT_YUV422P` 时，通过 RGA DMA fd 路径同步转换为 `RK_FMT_YUV420SP`；输出 MB 的 size/stride/virtual size 必须由 CAL API 计算。
- 每 100 帧同时记录 send/get、RGA convert 和 `VDEC + RGA` 端到端 avg/max 与实际 fps；连续 300 帧目标为接近输入 30fps且不持续积压。
- 首帧按 NV12 实际 stride 去除 padding 后保存，packed 文件应为 `1280 * 480 * 3 / 2 = 921600` bytes，并由用户人工确认方向、亮度和色彩。
- RGA 格式检查、转换、MB 获取、cache invalidate、输出布局或写入任一失败都返回非零；不使用 software fallback 掩盖硬件不支持。

### 修改文件

- `media_producer/uvc/uvc_vdec_probe.cpp`：新增可选 RGA 转换、NV12 MB pool、统计和首帧输出。
- `stream-server/CMakeLists.txt`：安装 probe 新增的直接运行依赖 `librga.so`。
- `memory-bank/*.md`：记录 Step 7 的职责边界、方案和验收结果。

### 禁止修改范围

- 不修改正式 `UvcH264Producer`、software decoder、VENC 或 mailbox。
- 不接入 dispatcher、distribution、HTTP、Web UI，也不实现 VDEC -> VENC bind/zero-copy。
- 不同时实现 RGA 和 VPSS；若实机证明 RGA 不支持该 planar YUV422P，再基于错误另立 VPSS 验证步骤。

### 验证

```bash
git diff --check
cmake --build stream-server/build/Debug
cmake --install stream-server/build/Debug
```

板端停止 `aipc` 后运行：

```bash
/root/aipc/bin/uvc_vdec_probe \
  --device /dev/video0 \
  --frames 300 \
  --convert-nv12 \
  --output /tmp/uvc_vdec_rga_first.nv12
```

用户确认连续性能、重复启停和 NV12 画面前，不将硬件链路并入正式 producer。

### 当前结果

- 独立 RGA probe 已实现，Debug 交叉构建、install、直接依赖/RPATH 检查和 `git diff --check` 均通过；安装包包含 `librga.so`。
- 首次自动部署尝试失败于 `ssh: connect to host 192.168.5.9 port 22: Connection refused`，尚无板端 RGA 能力、性能或画面结论。
