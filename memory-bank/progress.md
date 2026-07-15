# 双目 UVC 图像采集进度记录

## 当前状态

- 当前实施状态：双目 UVC Step 1、Step 3、Step 4、Step 5、Step 6、Step 7 均已通过板端验收。
- Step 1 已通过 Luckfox 板端实机验收。
- Step 2 已按用户要求取消，相关未交付代码已撤销。
- Step 3 已于 2026-07-13 由用户确认验收通过。

## 2026-07-14：Step 4 帧率优化阶段 1——分段性能统计

- 修改文件：
  - `stream-server/src/media_producer/uvc/uvc_producer.cpp`
  - `stream-server/src/media_producer/uvc/uvc_h264_producer.cpp`
  - `stream-server/src/media_producer/encoded_stream_dispatcher.h`
  - `memory-bank/PRD.md`
  - `memory-bank/design-document.md`
  - `memory-bank/tech-stack.md`
  - `memory-bank/implementation-plan.md`
  - `memory-bank/progress.md`
  - `memory-bank/architecture.md`
  - `memory-bank/code-design.md`
  - `memory-bank/dev-rules.md`
- 完成内容：
  - V4L2 capture 每 100 帧汇总 DQBUF 实际 fps、sequence gap 和 owned MJPEG copy 平均/最大耗时。
  - UVC H.264 input 每 100 个成功送帧汇总 decode、swscale、MB 获取、cache flush、VENC send 和总处理平均/最大耗时，并保留错误计数。
  - 共用 dispatcher 每 100 个输出帧汇总 VENC H.264 output fps。
  - 每次启动重置统计；未改变同步回调、解码、MB/VENC 所有权、consumer 调度和 HTTP 行为。
- 自动验证：`git diff --check`、`cmake --build stream-server/build/Debug` 均通过，`aipc` 和 `uvc_capture_test` 链接成功。
- 待验收：板端 UVC 模式至少运行 300 帧，核对 capture/input/output fps、sequence gap、各段 avg/max 耗时，以及 RTSP/WebRTC/WebSocket Preview 启停行为。
- 下一步门禁：用户确认板端统计日志有效前，不实施 bounded latest-frame queue。
- 首次板端结果：500 帧时 capture/input/output 为 `13.52/13.53/13.53fps`，sequence gap 603；decode、swscale、总耗时平均为 `24.51/48.26/73.61ms`，MB/flush/VENC send 不是主要瓶颈。
- 首次复测暴露 swscale 对 legacy YUVJ pixel format 的逐帧 range warning；当前继续在 Step 4 内显式修正 JPEG full-range -> limited-range NV12，修正后需再次板端确认 warning、色彩和统计结果。
- range 修正自动验证：legacy YUVJ format 映射和 `sws_setColorspaceDetails` 配置已实现，`git diff --check` 与 Debug 交叉构建通过；未修改同步数据流或后续 queue 逻辑。
- 修正后板端结果：300 帧时 capture/input/output 为 `13.84/13.81/13.81fps`，未再出现 deprecated pixel-format warning；HTTP 三个状态接口均返回 200。Step 4 验收通过。

## 2026-07-14：Step 5 帧率优化阶段 2——采集与转码解耦

- 修改文件：
  - `stream-server/src/media_producer/uvc/uvc_h264_producer.cpp`
  - `memory-bank/design-document.md`
  - `memory-bank/implementation-plan.md`
  - `memory-bank/progress.md`
  - `memory-bank/architecture.md`
  - `memory-bank/code-design.md`
  - `memory-bank/dev-rules.md`
- 完成内容：
  - capture callback 改为只替换容量 1 的 pending latest frame 并通知处理线程。
  - 独立处理线程串行复用既有 FFmpeg decoder、swscale、RKMPI MB pool 和 VENC channel。
  - pending frame 已存在时主动丢弃旧帧，累计 mailbox dequeued/drop、current/max depth 和 wait avg/max。
  - 启动顺序为 dispatcher -> processing -> capture；停止顺序为 reject -> stop capture -> clear/join processing -> stop dispatcher。
  - 未修改通用 `UvcProducer`、distribution、HTTP、Web UI、decoder/VENC 参数或媒体类型语义。
- 自动验证：`git diff --check` 与 `cmake --build stream-server/build/Debug` 通过，`aipc` 链接成功。
- 待验收：板端至少 500 capture 帧，验证 capture 接近 30fps、sequence gap 显著下降、mailbox 主动 drop、等待时间不累积，以及启动/停止/重复启停和三种网络输出。
- 下一步门禁：用户确认 Step 5 板端通过前，不建立 RKMPI MJPEG VDEC probe。
- 板端验收结果：500 个 capture 帧稳定 `30.07fps`、sequence gap 0；mailbox depth `1/1`、wait avg/max `17.08/35.27ms`，200 个 output 时 drops 256、output `13.16fps`。Step 5 验收通过。

## 2026-07-14：Step 6 帧率优化阶段 3——RKMPI MJPEG VDEC 独立 probe

- 修改文件：
  - `stream-server/src/media_producer/uvc/uvc_vdec_probe.cpp`
  - `stream-server/src/media_producer/uvc/CMakeLists.txt`
  - `stream-server/CMakeLists.txt`
  - `memory-bank/implementation-plan.md`
  - `memory-bank/progress.md`
  - `memory-bank/architecture.md`
  - `memory-bank/code-design.md`
  - `memory-bank/tech-stack.md`
- 完成内容：
  - 新增独立 `uvc_vdec_probe`，复用真实 `UvcProducer` owned MJPEG frame，不接入正式 H.264 producer 或 distribution。
  - 建立 RKMPI SYS/VDEC channel 0 生命周期，按 `VIDEO_MODE_FRAME + RK_VIDEO_ID_MJPEG` 同步执行 Send/Get/Release，并验证实际 pixel format、尺寸、stride、virtual size 和 MB size。
  - external MB free callback 持有 `UvcFramePtr`；首个 decoded frame 按实际 NV12 或 YUV422P plane layout 去除 stride 后保存。
  - 每 100 帧输出 decoded fps 和 send/get/total avg/max，失败、格式不符和超时均返回非零。
  - probe 单独链接导出 VDEC API 的 `librockit_full.so`，安装包同步携带该库；正式 `aipc` 链接不变。
- 自动验证：Debug 全量交叉构建、`cmake --install stream-server/build/Debug` 和安装产物 ELF RPATH/DT_NEEDED 检查通过。
- 验收要求：停止 `aipc` 后板端连续解码 300 帧，按日志中的实际 pixel format 人工检查首帧，并重复运行至少 3 次。
- 首次板端反馈实际启动的是正式 `aipc`，日志包含 `main.cpp`、`UVC-H264` 和 `EncodedDispatcher`，没有 `UVCVdecProbe/MJPEG VDEC`，因此不计入 Step 6 验收。该次运行在 capture 100 帧、`30.07fps`、sequence gap 0 后发生内核级 `usb 1-1: USB disconnect`，随后 V4L2 poll 返回 `0x18`；这是设备/USB 链路断开，不是 VDEC probe 结果。
- 后续板端诊断确认 USB host 多次读取 device descriptor 超时（`error -110`），port power cycle 后仍 `unable to enumerate USB device`；`v4l2-ctl --list-devices` 中只剩 `rkisp/rkcif` 节点，`/dev/video0` 不存在。Step 6 当前被相机物理连接/供电/USB 枚举状态阻塞，不能改用 `/dev/video13` 等 MIPI 节点替代。
- 相机重新连接后的正式 `aipc` 日志已连续达到 600 capture 帧，保持 `30.06~30.07fps`、sequence gap 0，未再次出现 USB disconnect；物理 USB 枚举阻塞已解除。200 个 software H.264 output 为 `13.70fps`，mailbox depth `1/1`、drops 239、wait avg/max `15.97/35.13ms`，再次确认 Step 5 行为稳定，但该日志仍不是 Step 6 probe 结果。
- 同次运行继续到 700 capture 帧后再次发生内核级 `usb 1-1: USB disconnect`，随后 descriptor read 持续 `error -110`；应用收到 `POLLERR|POLLHUP (0x18)` 的时间晚于内核断连，证明停止采集不是 mailbox、swscale 或 VENC 主动触发。Step 6 前置条件改为先用独立 `v4l2-ctl` 连续采集至少 1000 帧，隔离验证 USB/供电/线材/相机稳定性。
- 源码复核发现掉线后的健壮性缺口：`UvcProducer` capture thread 会设置自身 `running=false`，但 `UvcH264Producer::running_`、processing thread 和 dispatcher 不会随之收敛，导致 HTTP server 继续响应、producer 状态可能失真且 VENC fetch 持续超时。该问题与 USB 断连根因分开记录，后续需单独设计失败状态传播/受控停止或重连，不能在 Step 6 probe 中顺带修补。
- 同次日志中首帧 swscale colorspace 配置失败后，下一帧配置成功；现有代码失败即丢弃当帧，未输出错误转换帧。该现象暂记为正式 software 链路的独立诊断项，不在 Step 6 内修改。
- 首次真正运行 `uvc_vdec_probe` 已成功产出一帧无压缩 `1280x480`，但实际 format 7 为 `RK_FMT_YUV422P`、MB size 1228800 bytes；原 probe 只接受 NV12，因此在格式校验处主动退出。该结果证明 VDEC 能解码，但不能证明直接得到 VENC 所需 NV12。
- probe 已局部修正为逐帧接受/校验实际 NV12 或 YUV422P；YUV422P 按 Y/U/V 三 plane、各 chroma stride 为 luma stride 的一半保存 packed frame，并在日志输出实际格式、block size 和 packed size。Debug 全量交叉构建、install、`git diff --check` 均通过。
- 修正后首次连续运行完成 300 帧：decoded `29.84fps`，send/get/total avg 为 `0.161/24.643/24.810ms`、max 为 `3.751/77.519/77.615ms`；capture `29.84fps`、sequence gaps 2。输出稳定为 YUV422P `1280x480`，stride 1280，block/packed 均为 1228800 bytes；VDEC format/size/unsupported/packet/stream-too-large errors 全为 0，正常 stop/release 并返回 completed。
- 随后两次重复运行均完整完成 300 帧：repeat1/repeat2 最终 VDEC 为 `29.96/29.85fps`，total avg/max 分别为 `24.719/56.001ms` 与 `24.576/84.954ms`，capture sequence gaps 为 0/1；两次格式、尺寸、stride、文件大小和零错误状态均一致，正常 stop/release，未发生 USB disconnect。
- 用户已确认 YUV422P 画面方向、亮度和色彩正常。Step 6 的性能、三次连续运行、生命周期、输出格式和人工画面验收全部通过。
- 下一阶段候选为独立验证 YUV422P -> NV12 硬件转换能力和端到端帧预算；尚未开始，不直接修改正式 `UvcH264Producer`，也不在没有 probe 数据时选择 RGA/VPSS 或 bind/zero-copy 方案。

## 2026-07-15：Step 7 帧率优化阶段 4——RGA YUV422P 转 NV12 独立 probe

- 当前目标：在 Step 6 probe 内以显式模式验证 RGA DMA fd 转换，记录 VDEC、RGA 和端到端耗时，保存 packed NV12 首帧。
- 设计选择：先只验证 RGA，不并行实现 VPSS；RGA 不支持 planar YUV422P 时明确失败并以实机错误作为下一方案依据。
- 边界：不修改正式 `UvcH264Producer`、VENC、distribution 或 HTTP；板端通过前不接入正式数据流。
- 当前状态：独立 RGA probe 已实现并通过 Debug 交叉构建；安装包显式携带新增的直接依赖 `librga.so`，等待板端验收。
- 部署尝试：固定地址 `root@192.168.5.9` 的 SSH 端口拒绝连接，未修改板端文件；连接恢复后从部署和 300 帧 probe 继续。
- 首次板端结果：300 帧完成，capture/VDEC+RGA 为 `30.07/30.10fps`、sequence gap 0；RGA avg/max `1.229/2.314ms`，pipeline avg/max `21.401/46.818ms`，VDEC 错误计数全 0，停止和资源释放正常。
- 输出 NV12 为 `1280x480`、stride 1280、block/packed 921600 bytes。首次性能和文件布局门禁通过；等待两次重复运行及用户人工确认画面后再完成 Step 7。
- Codex 通过固定板端 SSH 独立完成后续两次 300 帧运行，远程退出码均为 0：repeat1/repeat2 最终 `30.05/30.03fps`，capture `30.03/30.02fps`，sequence gap 均为 0；RGA avg/max 为 `1.236/2.106ms` 与 `1.234/2.189ms`，pipeline avg/max 为 `21.861/73.228ms` 与 `21.807/61.188ms`，VDEC 错误计数全 0并正常释放。
- repeat2 NV12 已拉回本机，实际大小 921600 bytes，并以 `nv12 1280x480` 转为 PNG 检查：左右目完整且拼接边界正确，无 U/V 错位、绿紫偏色、行错位或拉伸；场景显示器高光过曝在两目一致，非转换异常。Step 7 全部门禁通过。

## 2026-07-14：项目协作说明维护

- 结合当前源码、CMake preset、部署脚本、Web 前端、Vision Client 和本 memory-bank 更新根目录 `AGENTS.md`。
- 补充已落地的 SimpleIPC/UVC producer、UVC H.264 数据流、目录职责、分范围验证命令和板端验收路径。
- 本次仅维护协作说明，不改变业务代码、HTTP API、模块职责或依赖方向。
- 验证：`git diff --check`；对未跟踪的 `AGENTS.md` 额外执行 `git diff --no-index --check /dev/null AGENTS.md`。

## 历史背景

- 仓库中的 `AGENTS.md` 记录了既有 stream server review 重构 Step 1-6 的约束，但本次开始时 `memory-bank/` 目录不存在。
- 本文件从双目 UVC 功能开始恢复 memory-bank 记录；不反向宣称未能从文件核验的旧步骤测试结果。

## Step 1：独立 UVC/V4L2 MJPEG 采集闭环

- 完成时间：2026-07-13
- 修改文件：
  - `memory-bank/PRD.md`
  - `memory-bank/design-document.md`
  - `memory-bank/tech-stack.md`
  - `memory-bank/implementation-plan.md`
  - `memory-bank/progress.md`
  - `memory-bank/architecture.md`
  - `memory-bank/code-design.md`
  - `memory-bank/dev-rules.md`
  - `stream-server/src/media_producer/CMakeLists.txt`
  - `stream-server/src/media_producer/uvc/CMakeLists.txt`
  - `stream-server/src/media_producer/uvc/uvc_config.h`
  - `stream-server/src/media_producer/uvc/uvc_producer.h`
  - `stream-server/src/media_producer/uvc/uvc_producer.cpp`
  - `stream-server/src/media_producer/uvc/uvc_capture_test.cpp`
- 完成内容：
  - 恢复并初始化 memory-bank 工作流文档。
  - 实现显式节点和 `/dev/video*` 自动发现共用的 UVC capability/format/size/fps 校验。
  - 实现 V4L2 mmap 初始化、stream/thread 生命周期、owned MJPEG frame 和类型化 callback。
  - 实现 `uvc_capture_test`，可采集指定帧数并保存第一帧 JPEG。
  - 保持现有 `IMediaProducer`、SimpleIPC、distribution 和 HTTP API 不变。
- 自动测试方式：

```bash
git diff --check
cmake --build stream-server/build/Debug
```

- 自动测试结果：两条命令均通过；`uvc_lib` 和 `stream-server/build/Debug/bin/uvc_capture_test` 交叉编译、链接成功。
- 板端测试结果：2026-07-13 自动选择 `/dev/video0`，成功初始化 4 个 mmap buffer，采集 100 帧并生成 `/tmp/uvc_first.jpg`；首帧 14078 bytes、`1280x480`。
- 性能观察：从用户日志时间估算，100 帧约耗时 9.6 秒（约 10.4 fps），低于配置的 30 fps；诊断程序已补充实测 fps 输出，后续需结合 `v4l2-ctl` 和 USB 链路确认原因。
- 给下一位开发者的提醒：MJPEG 不得直接接入现有 H.264 consumer；优先记录 software decode 和 VENC 的实际帧率再决定是否替换硬件 decoder。

## Step 3：UVC H.264 与现有三种网络分发接入

- 完成时间：2026-07-13
- 完成内容：
  - 用户取消 Step 2，已撤销独立 MJPEG WebSocket 和 PyQt 客户端代码。
  - 提取 SimpleIPC/UVC 共用的 `EncodedStreamDispatcher` 和 H.264 VENC 配置 helper。
  - 新增 `UvcH264Producer`：FFmpeg MJPEG decode、swscale NV12、RKMPI MB pool/VENC。
  - `MediaManager` 新增 `ProducerMode::UvcH264`；`aipc` 支持 `--mode uvc`。
  - 现有 RTSP/WebRTC/H.264 WS/File consumer 注册和 `StreamManager` 所有权不变。
  - 修正 `assets/start_app.sh` 与当前 CLI 的偏差：支持 `--mode uvc`，并保持脚本默认启动 RTSP、WebRTC、WebSocket Preview 的原有习惯。
  - 板端首次运行暴露 `libdatachannel.so.0.24` 缺失；确认 `aipc` 的 ELF `DT_NEEDED` 和 `$ORIGIN/../lib` RPATH 后，修正 `install_rsync.sh` 默认保留并部署该库。
  - HTTP endpoint path、原 JSON 字段和既有 message 保持；仅扩展 mode 值、available modes 和 UVC 状态内容。
  - `uvc_capture_test` 增加实测 fps 日志；V4L2 owned frame 复制后先 QBUF 再执行下游回调。
- 自动验证：

```bash
git diff --check
cmake --build stream-server/build/Debug
cmake --install stream-server/build/Debug
```

- 自动结果：交叉编译和链接通过；板端 install 产物为 `stream-server/build/Debug/install/bin/aipc`。
- 板端验收结果：用户确认 Step 3 通过；`start_app.sh --mode uvc` 可启动 UVC H.264 链路并复用 RTSP、WebSocket Preview 和 WebRTC。
- 性能记录：纯 V4L2 输入约 `29.1fps`，当前软件 MJPEG decode + swscale 完整输出约 `14fps`；这是后续性能优化项，不影响本步骤功能完成状态。
