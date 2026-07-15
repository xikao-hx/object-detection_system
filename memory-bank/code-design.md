# Step 1 代码设计：独立 UVC/V4L2 MJPEG 采集

## 当前功能

- 功能名称：USB UVC 双目 MJPEG 采集
- 所属实施步骤：Step 1
- 关联需求：自动发现、mmap、`1280x480@30 MJPG`、类型化帧回调、板端诊断

## 成功标准

- 行为成功标准：自动/显式选择合格节点并连续采集 100 帧。
- 结构成功标准：UVC 独立于 H.264 producer/distribution，不伪造 `VENC_STREAM_S`。
- 验证成功标准：交叉构建通过；板端生成可解码的左右拼接 JPEG。

## 现有代码观察

- 相关目录：`media_producer/simple_ipc/`、`media_producer/i_media_producer.h`。
- 现有主流程：SimpleIPC 生成 H.264 后经 `MediaManager` 注册到 distribution consumers。
- 可复用模块：`common/logger.h` 与 CMake 子模块组织。
- 需要避免触碰的旧逻辑：`main.cpp`、HTTP handler、`MediaManager`、RTSP/WebRTC/File/WS 实现。

## 修改文件

- `stream-server/src/media_producer/CMakeLists.txt`：纳入 UVC 子目录。
- `stream-server/src/media_producer/uvc/CMakeLists.txt`：定义 `uvc_lib` 和诊断程序。
- `stream-server/src/media_producer/uvc/uvc_config.h`：定义 UVC 专属配置。
- `stream-server/src/media_producer/uvc/uvc_producer.h`：定义帧、回调和生命周期接口。
- `stream-server/src/media_producer/uvc/uvc_producer.cpp`：集中实现发现、ioctl/mmap 和采集线程。
- `stream-server/src/media_producer/uvc/uvc_capture_test.cpp`：板端黑盒验收入口。

## 禁止修改范围

- `i_media_producer.h`：当前 H.264 接口不能承载 MJPEG。
- `media_manager.*`：Step 1 不做模式切换和传输集成。
- `media_distribution/`、`http.*`、`main.cpp`：Step 1 不改变服务/API 行为。

## 职责划分

- `uvc_config.h`：只保存采集参数与默认值。
- `uvc_producer.*`：设备能力校验、V4L2 资源生命周期、帧所有权和回调调度。
- `uvc_capture_test.cpp`：参数解析、等待验收帧数和保存首帧，不复制 V4L2 逻辑。
- 不允许 UVC producer 承担网络发送、JPEG 解码、左右拆分或 H.264 编码。

## 依赖方向

- 允许：诊断程序 -> `UvcProducer` -> Linux V4L2/common logger。
- 禁止：UVC producer -> HTTP/MediaManager/StreamManager/distribution service。
- 禁止：SimpleIPC 与 UVC 相互依赖。

## 数据流/状态流

1. 输入来自 V4L2 mmap buffer。
2. buffer 有效区在 producer 内复制到 `std::vector<uint8_t>`。
3. initialized/running 和 fd/mmap 资源只由 producer 生命周期维护。
4. owned `UvcFrame` 输出到启动前注册的回调。

## 接口设计

- `FindUvcDevice(config)`：返回首个满足配置的节点或空字符串。
- `SetFrameCallback(callback)`：仅非 running 时成功。
- `Init()/Deinit()/Start()/Stop()`：显式生命周期。
- `UvcFrame`：bytes、width、height、sequence、timestamp_us。

## 方案取舍

- 采用：UVC 专属 producer API。
- 不采用：扩展现有 `StreamCallback` 为 variant 或 `void*`。
- 不采用原因：会扩散类型判断，破坏 H.264 consumer 契约，也违反不新增 `void* user_data` 路径的项目约束。

## 风险与约束

- 最容易写脏的点：发现与初始化各自重复 capability 判断；通过单一 probe helper 避免。
- mmap 风险：每个成功映射的 buffer 必须在任何失败路径中解除。
- 停止风险：`VIDIOC_STREAMOFF` 用于唤醒 poll，join 后才关闭 fd。
- callback 风险：捕获异常并继续 requeue；禁止在 callback 线程内销毁 producer。
- 兼容性风险：当前步骤不修改 HTTP API 和现有 H.264 行为。

## 结构自查清单

- 是否只修改必要文件：是。
- 是否复用现有模式：复用 logger/CMake 子模块，不复用不兼容的 H.264 类型。
- 是否避免重复逻辑：显式设备和自动发现共享 probe。
- 是否避免越层调用：是。
- 是否没有提前实现后续步骤：是。

## 验证方式

- 自动：`git diff --check`；`cmake --build stream-server/build/Debug`。
- 板端：`./uvc_capture_test --frames 100 --output /tmp/uvc_first.jpg`。
- 预期：日志显示选中 UVC 节点和 100 帧统计，输出 JPEG 为 `1280x480`。

## 实施结果

- 代码结构按本设计落地，未触碰禁止修改范围。
- Debug 交叉构建已通过，产物位于 `stream-server/build/Debug/bin/uvc_capture_test`。
- HTTP path、响应 message 和 JSON 字段无修改，完全保持兼容。
- Luckfox 实机采集已通过：自动选择 `/dev/video0`、100 帧、首帧 14078 bytes、`1280x480`。

# Step 3 代码设计：UVC H.264 与现有三种网络分发接入

## 成功标准

- 行为成功标准：`start_app.sh --mode uvc` 自动采集 MJPEG、生成 H.264 Annex-B，并送入原 RTSP、WebRTC、WebSocket H.264 三种网络输出。
- 结构成功标准：UVC producer 实现 `IMediaProducer` 并输出既有 `EncodedStreamPtr`；各 distribution service 和 `StreamManager` 不感知输入来自 UVC。
- 验证成功标准：Debug 交叉构建通过，板端 RTSP 可播放、WebSocket Preview 可连接、WebRTC 保持原控制路径。

## 修改文件

- `media_producer/encoded_stream_dispatcher.h`：复用 H.264 fetch 和 typed consumer 分发。
- `simple_ipc/simple_ipc_producer.cpp`：改用共用 dispatcher，不改变行为。
- `uvc/uvc_h264_config.h`：组合 UVC capture 和 H.264 参数。
- `uvc/uvc_h264_producer.*`：FFmpeg MJPEG decode、swscale NV12、MB pool、RKMPI VENC。
- `uvc/CMakeLists.txt`、`media_producer/CMakeLists.txt`：独立 producer target 和链接依赖。
- `i_media_producer.h`、`media_manager.*`：新增 UVC H.264 factory/mode。
- `main.cpp`、`http.cpp`：启动 mode 与兼容 API 状态。
- `assets/start_app.sh`：将原有默认启用语义转换为当前 `aipc --rtsp/--webrtc` 参数，并透传 `--mode uvc`。
- `assets/install_rsync.sh`：默认部署 `aipc` 的直接动态库依赖 `libdatachannel.so.0.24`；仅在板端系统目录已预装兼容库时允许显式跳过。

## 禁止修改范围

- `media_distribution/rtsp/`：不修改；继续只接收 H.264 typed stream。
- `StreamManager`：不新增 UVC 所有权或 producer 依赖。
- `vision-client/`：按用户要求暂不处理 PyQt 推理。

## 依赖和数据流

```text
/dev/videoX -> UvcProducer -> owned MJPEG frame
            -> FFmpeg MJPEG decode -> swscale NV12
            -> RKMPI VENC H.264 -> EncodedStreamDispatcher
            -> MediaManager registered consumers
            -> RTSP / WebRTC / WebSocket Preview / File
```

- 允许：UvcH264Producer -> UvcProducer/FFmpeg/RKMPI/common dispatcher。
- 禁止：UvcH264Producer -> RTSP/HTTP/StreamManager。
- 禁止：MJPEG bytes 直接进入 `EncodedStreamPtr`。

## 方案取舍

- 采用 FFmpeg software MJPEG decoder：rootfs/工程已有 FFmpeg，能最快验证完整 H.264 分发链路；性能作为板端验收数据记录。
- 采用 swscale 直接写 RKMPI MB 的 NV12 planes：避免中间 NV12 vector 拷贝。
- 采用 RKMPI VENC：生成与现有 consumer 完全一致的 H.264 stream。
- 不采用 MJPEG 冒充 H.264、不修改 RTSP payload、不在客户端临时转码。

## 风险与避免方式

- FFmpeg decode 速度：同步处理并记录实际 fps；若低于需求，后续只替换 decoder，不改 producer/consumer 边界。
- VENC MB 生命周期：从预分配 pool 取 block，flush 后 SendFrame，随后释放调用方引用。
- V4L2 buffer 饥饿：MJPEG owned copy 完成后先 QBUF，再执行 decode callback。
- shutdown：先停 V4L2 输入，再停 H.264 fetch，销毁 VENC/MB pool/decoder，最后退出 MPI。
- 部署依赖：`aipc` 即使只启动 RTSP，也会在 ELF 装载阶段解析已链接的 WebRTC 动态库；不能根据运行参数删除 `libdatachannel`。

## 实施结果

- Step 2 代码已完全撤销，`vision-client` 和独立 MJPEG service 无残留。
- `UvcH264Producer` 已实现 `IMediaProducer`，distribution 层无需识别 UVC。
- SimpleIPC 和 UVC 已共用 encoded dispatcher 与 VENC setup helper，未复制 H.264 fetch/配置逻辑。
- Debug 交叉构建通过；三种网络输出共用同一 UVC H.264 stream，板端行为已由用户确认验收通过。
- 修复部署包默认删除 `libdatachannel` 导致程序尚未进入 `main()` 就启动失败的问题。
- 功能验收时完整链路约 `14fps`；已记录为 software MJPEG decode/swscale 的性能边界，不改变 producer/distribution 职责划分。

# Step 4 代码设计：分段性能统计

## 成功标准

- `UvcProducer` 每 100 个有效 DQBUF 汇总实际 fps、sequence gap、copy 平均/最大耗时。
- `UvcH264Producer` 每 100 个成功送入 VENC 的帧汇总 input fps，以及 decode、swscale、MB 获取、cache flush、VENC send 的平均/最大耗时和既有错误计数。
- `EncodedStreamDispatcher` 每 100 个 H.264 输出帧汇总实际 output fps。
- 统计在每次 `Start()` 时重新开始，停止/重复启动不会沿用上一次窗口。

## 修改文件与职责

- `uvc_producer.cpp`：只观察 V4L2 capture 边界；不感知 decoder 或 distribution。
- `uvc_h264_producer.cpp`：只观察 UVC 转码内部阶段；不改变处理顺序和资源所有权。
- `encoded_stream_dispatcher.h`：只观察共用 VENC fetch 边界；不区分 producer 类型，不改变 consumer 调度。

## 依赖和调用方向

依赖方向保持 `UvcProducer -> typed callback -> UvcH264Producer -> EncodedStreamDispatcher -> consumers`。计时使用 `std::chrono::steady_clock` 和各模块私有累计值，不新增公共 helper、回调参数或跨层状态查询。

## 禁止修改范围和方案取舍

- 不实现 queue、worker、VDEC、zero-copy 或 bind，避免将测量与行为优化混在同一步。
- 不把统计字段加入 HTTP JSON，保持 API 完全兼容。
- 不逐帧记录 INFO；选择每 100 帧累计平均值和最大值，在可读性与观测开销间保持平衡。
- 本步暂不报告 queue depth/drop/wait，因为队列尚不存在；下一阶段随 bounded queue 一并增加。

## 风险与避免方式

- sequence 回绕：以无符号 delta 判断前向跳号，过滤异常反向值。
- 失败帧导致统计不输出：采集统计按有效 DQBUF 计数；转码耗时按成功送帧汇总，同时保留 decode/send error 计数。
- 重复启动污染统计：在 `Start()` 前清零模块私有累计值。
- 统计本身扰动性能：热路径只读取 steady clock 和累加数值，格式化日志每 100 帧执行一次。
- MJPEG 色彩范围：FFmpeg 可能输出 deprecated YUVJ format；使用同 plane layout 的非 J pixel format 创建 swscale context，并通过 `sws_setColorspaceDetails` 显式声明 BT.601 JPEG full-range 输入和 limited-range NV12 输出，不通过日志过滤掩盖问题。

## 验证方式

- 自动：`git diff --check`；`cmake --build stream-server/build/Debug`。
- 板端：UVC 模式持续运行至少 300 帧，检查 capture/input/output 三组 fps 与各段 avg/max 日志；验证 RTSP、WebRTC、WebSocket Preview 行为不变。

## 实施结果

- 三个既有数据边界已分别增加模块私有统计，没有新增公共接口或跨层依赖。
- 统计在 `Start()` 时重置，每 100 帧汇总；当前仍同步执行 `CaptureLoop -> EncodeFrame`。
- `git diff --check` 与 Debug 交叉构建通过；板端统计有效性和运行行为待用户验收。
- 首次板端 500 帧统计已成功定位瓶颈，但出现逐帧 deprecated pixel format/range warning；Step 4 增加一次局部色彩范围修正并要求复测，尚不进入 queue 阶段。
- legacy YUVJ 映射和显式 BT.601 JPEG full-range -> limited-range NV12 配置已通过 Debug 交叉构建；等待板端确认 warning 消失且画面亮度、黑位和色彩正常。
- 修正后 300 帧日志未再出现 deprecated pixel-format warning，三组 fps 和 HTTP 状态正常；Step 4 验收通过。

# Step 5 代码设计：容量 1 的 latest-frame mailbox

## 成功标准

- capture callback 的热路径仅执行 mutex、替换一个 owned `UvcFramePtr` 和 notify，不执行 decode/sws/VENC。
- mailbox 同时最多持有一个待处理帧；新帧覆盖旧待处理帧时累计主动 drop。
- 独立处理线程串行复用现有单 decoder context、sws context、MB pool 和 VENC channel。
- stop/restart 不遗留线程或待处理帧；每次启动重置 mailbox 和统计。

## 修改文件与职责

- `uvc_h264_producer.cpp`：在既有 `Impl` 内管理 pending frame、mutex、condition variable、处理线程及 mailbox 统计。
- `uvc_producer.cpp` 不修改：继续只负责设备、mmap、owned copy 和 callback。
- `encoded_stream_dispatcher.h` 不修改：继续只负责 H.264 fetch 和 consumer 分发。

## 数据流和生命周期

```text
capture thread: DQBUF -> owned copy -> QBUF -> replace pending latest frame -> notify
processing thread: wait -> take pending frame -> decode -> swscale -> VENC_SendFrame
stop: reject frames -> stop/join capture -> clear/wake/join processing -> stop dispatcher
```

- 启动顺序：dispatcher -> processing thread -> accept frames -> capture。
- 失败回滚：capture 启动失败时先拒绝帧，再停止处理线程，最后停止 dispatcher。
- 停止时清空 pending frame，不 drain 历史帧，保证 shutdown 和实时性。

## 方案取舍

- 采用容量 1 mailbox，而不是通用队列类：当前只有单 producer 调用方，容量 1 直接表达 latest-frame 策略且没有虚构复用价值。
- 不增加配置项：本步骤固定容量 1，避免扩展 CLI/HTTP/API；实测证明有调整需求后再设计参数。
- 保持单处理线程：FFmpeg decoder/sws/VENC 资源继续由单线程串行访问，不引入 context 竞争和输出乱序。

## 风险与避免方式

- shutdown 竞态：先停止 capture 确保不再 enqueue，再清空 mailbox、通知并 join processing thread。
- 线程创建失败：恢复 processing 状态并返回启动失败，不启动 capture。
- 处理异常：在线程入口捕获异常、记录并继续，不能导致 `std::terminate`。
- 统计竞争：mailbox drop/depth/wait 全部在同一 mutex 下维护，日志只读取快照。
- 预期边界：本步改善 capture fps 和延迟，不承诺 software H.264 output 超过约 14fps。

## 验证方式

- 自动：`git diff --check`；`cmake --build stream-server/build/Debug`。
- 板端：至少 500 capture 帧；预期 capture 接近 30fps、sequence gap 显著下降、mailbox drops 增长、深度始终为 1、wait 不持续累积，output 仍接近 software 上限。

## 实施结果

- mailbox、processing thread、主动 drop 和 wait/depth 统计均封装在 `UvcH264Producer::Impl`，没有新增公共接口或配置项。
- capture callback 已不再执行 decode/sws/VENC；原 decoder、MB 和 VENC 所有权及错误日志保持。
- Debug 交叉构建通过；板端性能和生命周期行为等待用户验收。
- 板端 500 capture 帧验证 capture `30.07fps`、sequence gap 0、mailbox 有界且等待不累积；Step 5 验收通过。

# Step 6 代码设计：RKMPI MJPEG VDEC 独立 probe

## 成功标准

- 使用真实 `UvcProducer` owned MJPEG frame，而不是仓库样例或合成 JPEG。
- 连续完成目标数量的 `SendStream -> GetFrame -> ReleaseFrame`，输出尺寸必须为 `1280x480`；pixel format 接受请求得到的 NV12 或硬件返回的 `RK_FMT_YUV422P`，但必须逐帧准确校验。
- 每 100 帧汇总 send/get/total avg/max 和实际 decoded fps；首帧按实际 plane layout 与 visible width/height 去除 stride 后保存为 packed NV12 或 packed YUV422P。
- 初始化失败、单帧失败、超时或输出不符合预期时明确返回非零；退出后 VDEC channel 和 MPI system 完整释放。

## 修改文件与职责

- `uvc_vdec_probe.cpp`：CLI、RKMPI VDEC 生命周期、external MB 输入所有权、decoded frame 校验/保存和统计。
- `uvc/CMakeLists.txt`：probe 编译链接，不把 probe 代码加入 `uvc_lib` 或 `uvc_h264_lib`。
- 根 `CMakeLists.txt`：将 probe 安装到现有 bin 目录，并携带板端通常未预装的 `librockit_full.so`。

## 依赖和数据流

```text
/dev/videoX -> UvcProducer owned MJPEG
  -> external MB retaining UvcFramePtr
  -> VDEC SendStream(frame mode, MJPEG)
  -> VDEC GetFrame(actual NV12 or YUV422P)
  -> validate actual layout/save/release
```

- 允许：probe -> `UvcProducer`、RKMPI SYS/MB/CAL/VDEC、common logger。
- 禁止：probe -> `UvcH264Producer`、VENC、MediaManager、HTTP、StreamManager、distribution。

## 方案取舍

- 采用独立 probe 而不是直接替换 decoder：先隔离固件/API/码流兼容性变量。
- 每帧同步 Send/Get：最小化输入与输出配对歧义，直接测出硬件单帧能力；若达不到 30fps，再根据 status 判断是否需要异步 pipeline。
- input external MB 的 free callback 持有 `UvcFramePtr`，Send 后释放调用方 MB 引用，避免复制或悬空 mmap/owned bytes。
- 首帧保存实际 decoded format，不直接假设 stride 等于 width。NV12 按 Y/UV 两 plane，YUV422P 按 Y/U/V 三 plane；均按 stride、virtual height 和 MB size 校验后逐行写出。
- 不在 probe 中把 YUV422P 转为 NV12：当前步骤只确认 decoder 能力和性能；RGA/VPSS 转换属于获得连续实测结果后的下一设计决策。

## 风险与避免方式

- VDEC channel 冲突：probe 运行前停止 `aipc`，固定使用 channel 0，不与正式服务并行。
- input 生命周期：external MB opaque holder 直到 RKMPI 释放时才销毁 owned frame。
- CPU 读取输出：保存前以 read-only 方向 flush/invalidate cache。
- partial init：按 StartRecvStream -> DestroyChn -> SYS_Exit 逆序清理，所有失败路径复用同一生命周期对象。
- callback 线程停止：达到目标后只通知主线程，由主线程调用 `producer.Stop()`，禁止 callback 自己 join。

## 验证方式

- 自动：`git diff --check`、Debug 交叉构建和 install。
- 板端：连续 300 帧、按日志实际格式检查首帧、重复运行至少 3 次、异常 JPEG/timeout/shutdown 日志检查。

## 实施结果

- probe 与正式 producer 完全分离；`UvcH264Producer`、VENC、dispatcher、distribution 和 HTTP 未修改。
- SDK 导出符号确认 `RK_MPI_VDEC_*` 需要 `librockit_full.so`，因此仅 probe target 使用 `rockit_full/rockchip_mpp/drm/rga`；正式服务仍链接原 `rockit`。
- `bin/uvc_vdec_probe` 与 `lib/librockit_full.so` 已进入 install 目录，ELF RPATH 为 `$ORIGIN/../lib`。
- Debug 全量交叉构建和 install 已通过；板端真实连续解码、输出画面和重复运行待用户验收。
- 首次板端 probe 返回 `RK_FMT_YUV422P`、`1280x480`、virtual `1280x480`、MB `1228800`；原有“必须 NV12”假设已被实机证据推翻，当前局部修正只扩展准确校验/保存，不增加转换链路。
- NV12/YUV422P 分支已按各自 plane layout 落地，实际格式、block size、packed size 均写入首帧日志；Debug 全量交叉构建和 install 通过，等待板端连续 300 帧复测。
- 修正后板端首次连续完成 300 帧，VDEC `29.84fps`、total avg/max `24.810/77.615ms`、错误计数全 0，YUV422P packed 文件大小正确。
- 两次追加重复运行同样完成 300 帧并保持约 30fps、零 VDEC 错误、格式/大小一致和正常资源释放；三次运行门禁通过。
- 用户已确认 packed YUV422P 画面方向、亮度和色彩正常；Step 6 全部门禁通过。
- 本步不把 VDEC probe 代码并入正式 producer。后续若推进，先独立测量 YUV422P -> NV12 转换耗时和输出正确性，再决定正式 decoder 的局部替换设计。

# Step 7 代码设计：RGA YUV422P 转 NV12 独立 probe

## 成功标准

- 仅在显式 `--convert-nv12` 下启用，Step 6 原始输出检查模式保持兼容。
- 使用 `RK_MPI_CAL_COMM_GetPicBufferSize` 得到 NV12 MB size/virtual size，以私有 DMA pool 提供目标 buffer。
- VDEC 的 YUV422P MB fd 包装为 `RK_FORMAT_YCbCr_422_P`，目标 fd 包装为 `RK_FORMAT_YCbCr_420_SP`；先 `imcheck` 再同步 `imcvtcolor`。
- 每帧转换后及时释放目标 MB 和 VDEC frame；首帧在 cache invalidate 后按 stride 保存 packed NV12。
- 日志分别包含 VDEC send/get、RGA convert 和 pipeline total avg/max，不以输出 fps 单独掩盖阶段耗时。

## 数据流与所有权

```text
owned MJPEG -> external input MB -> VDEC YUV422P MB (VDEC owns)
  -> source DMA fd -> RGA -> private NV12 MB (probe owns)
  -> validate/save/release NV12 MB -> release VDEC frame
```

- 初始化顺序：SYS -> NV12 CAL/pool（转换模式）-> VDEC channel -> recv；清理严格逆序。
- 允许依赖：现有 probe 依赖加 librga im2d header/API；target 链接 `rga`，安装包显式携带其新增的直接依赖 `librga.so`。
- 禁止依赖：正式 `UvcH264Producer`、VENC、MediaManager、distribution 和 HTTP。

## 风险与处理

- planar YUV422P 支持取决于板端 RGA 驱动/库组合：`imcheck` 或 `imcvtcolor` 失败即终止并输出错误字符串，不做 software fallback。
- stride/virtual height 不可假设等于 visible size：source 使用 VDEC frame 实际值，destination 使用 CAL 结果。
- RGA 写后 CPU 读取首帧前执行 cache invalidate；目标 MB 始终在 VDEC frame 释放前完成同步转换。

## 验收门禁

- 自动：Debug 交叉构建、install、`git diff --check`。
- 板端：300 帧接近 30fps、输出 921600 bytes、人工画面正常，并至少重复运行 3 次验证资源释放。
- 验收前不修改正式 producer；验收后才设计 VDEC/RGA/VENC 的生产化生命周期和错误传播。

## 当前实施结果

- `--convert-nv12`、CAL 布局、私有 DMA pool、RGA `imcheck/imcvtcolor`、packed NV12 保存和分阶段统计已实现。
- Debug 交叉构建无告警，install 与 `git diff --check` 通过；ELF 直接依赖包含 `librga.so`，RPATH 为 `$ORIGIN/../lib`，安装包已携带该库。
- 固定板端 SSH 当前返回 connection refused，实机能力与性能门禁尚未执行。
- 后续首次实机运行连续完成 300 帧：RGA avg/max `1.229/2.314ms`，pipeline avg/max `21.401/46.818ms`，最终 `30.10fps`、sequence gap 0、VDEC 错误全 0；NV12 packed 输出为预期 921600 bytes。
- 当前只完成首次性能与布局验证；两次重复运行和画面方向/亮度/色彩人工确认仍是最终验收门禁。
- 两次追加运行均返回 0并保持约 30fps、sequence gap 0、RGA avg 约 `1.23ms`、pipeline avg 约 `21.8ms`、零 VDEC 错误和正常释放；三次运行门禁通过。
- 拉回的 repeat2 NV12 为准确的 921600 bytes；按 NV12 `1280x480` 转图后，双目水平拼接、亮度和色彩正常，无 plane/stride/几何异常。Step 7 验收完成，可进入正式 VDEC/RGA/VENC 生产化设计，但不得直接复制 probe 生命周期。

# Step 8 代码设计：正式 VDEC/RGA/VENC 接入

## 成功标准与职责

- `uvc_hardware_pipeline.*` 提供两个窄职责：按需加载并管理 full VDEC runtime/输出 frame 的 RAII release，以及 RGA NV12 MB 的 RAII release；调用方继续持有 compact `RK_MPI_SYS` 顶层生命周期供 VENC/RGA 使用。
- probe 保持原 CLI/文件输出；正式 producer 保持 mailbox、VENC frame metadata、dispatcher 和 consumer 语义。
- 正式统计改为 VDEC send/get、RGA、VENC send 和 pipeline total，仍每 100 个成功 VENC input 汇总。

## 数据流与所有权

```text
UvcFramePtr -> external MB(retain input) -> VDEC frame(move-only)
  -> RGA output MB(move-only) -> VENC SendFrame -> release caller MB ref
```

- `UvcH264Producer` 初始化：compact SYS -> shared full SYS/VDEC + RGA -> compact VENC；清理为 VENC -> shared hardware -> compact SYS。
- VDEC frame 必须在同步 RGA 完成后释放；NV12 MB 必须在 `VENC_SendFrame` 返回后释放调用方引用。
- RGA 写入是硬件到硬件，不执行 CPU write cache flush；只有 probe CPU 保存时 invalidate。

## 方案取舍与风险

- 不复制 probe：抽取共享组件，probe 也迁移使用，避免两套格式校验和资源清理漂移。
- 不保留自动 software fallback：同一运行只维护一套 decoder 状态，硬件错误明确计数和记录。
- 首次尝试让 `aipc` 统一链接 `rockit_full`，实机在 SimpleIPC 进入 RKAIQ 初始化后退出，因此否决。最终保留既有 `rockit` 作为唯一 `DT_NEEDED` 的公共 SYS/VENC/TDE 实现，UVC 组件使用 `dlopen(RTLD_LOCAL)` 和类型化 `dlsym` 按需取得 full 的 VDEC/CAL/SYS/MB API；加载失败或缺符号直接使 UVC 初始化失败，不静默 fallback。
- 不做 bind/zero-copy：当前 RGA output MB 直接作为 VENC input 已消除 CPU 像素转换，进一步绑定另立步骤。

## 验收门禁

- 自动：无告警交叉构建、install、`aipc` ELF 只出现 `librockit.so` 而不出现 `librockit_full.so` 的 `DT_NEEDED`、`git diff --check`。
- 板端：正式 UVC 超过 500 帧约 30fps、零硬件错误、RTSP/WS/WebRTC/HTTP、停止与重复启停均通过；共享 probe 追加 300 帧通过。当前板无 MIPI sensor，SimpleIPC 验证到既有 producer 初始化边界，并以 full runtime 未加载作为链接隔离门禁。
