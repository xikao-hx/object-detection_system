# 长期编码约束

## 使用方式

- 写代码前必须阅读本文档。
- 每完成一步更新 `progress.md`；职责变化同步更新 `architecture.md` 和 `code-design.md`。
- 本文档只记录长期规则，不记录单步进度。

## 通用原则

- 手术刀式修改，只碰当前目标必须触碰的文件。
- 优先复用仓库现有模式、目录职责和 helper。
- 不提前实现后续步骤，不做无关重构。
- 不回滚用户已有改动。

## 文件职责规则

- 参数、资源生命周期、业务传输和展示/算法保持分层。
- 公共 helper 只承载被多个模块稳定复用的逻辑；窄场景 helper 留在模块内部。
- 硬件/RKMPI/V4L2 setup helper 不依赖 HTTP、service manager 或 distribution service。

## 依赖方向规则

- `StreamManager` 是 distribution service 的唯一所有权边界。
- 禁止新增 `StreamManager` 外的全局 service 单例路径。
- 禁止底层 producer 反向依赖 HTTP 或 distribution。
- 禁止引入双向依赖或绕过现有主流程。

## 媒体数据规则

- H.264 consumer 使用 `EncodedStreamPtr`，非 H.264 数据不得伪装为 `VENC_STREAM_S`。
- 共享 H.264 Annex-B 解析必须复用 `common/h264_nal_parser.h`。
- UVC mmap 地址只在 dequeue/requeue 临界区有效；跨回调的数据必须有明确所有权。
- CPU 写入 RKMPI cached MB 后必须以 `RK_MPI_SYS_MmzFlushCache(block, RK_FALSE)` 提交给硬件。
- RKMPI 图像 buffer size/stride 必须使用 CAL API 计算，不能写死无对齐 NV12 大小。
- consumer/callback 必须在 producer start 前注册；运行中禁止新增或清理。
- 不新增 `void *user_data` 回调路径。

## 兼容性规则

- HTTP endpoint path、响应 message 和 JSON 字段默认保持兼容。
- 重构和行为变化分步进行；无法自动验证的板端行为必须给出人工验收命令。
- 录制时长限制必须关闭录制并记录原因，不能只输出 warning。

## 验证规则

- 修改 `stream-server/src/` 后至少执行 `git diff --check` 和 `cmake --build stream-server/build/Debug`。
- UVC 板端验收先确认实际板型、USB host 状态、`lsusb`、`v4l2-ctl --list-devices` 和格式能力。
- 部署统一二进制时以 ELF `DT_NEEDED` 为准准备动态库；不能因为某个运行参数暂未启用 WebRTC，就删除进程装载阶段仍必需的 `libdatachannel`。
- 性能观测使用 `steady_clock` 和低频汇总日志；禁止逐帧 INFO 日志改变被测媒体热路径负载。
- FFmpeg MJPEG legacy YUVJ 输入不得直接交给 swscale 或靠过滤日志隐藏 warning；应映射为同布局非 J format，并显式设置 JPEG full-range 和目标 range。
- 实时视频采集与慢处理解耦时使用有界 latest-frame 策略；队列满时丢旧帧保最新帧，禁止用无上限积压换取表面不丢帧。

## 禁止事项

- 禁止复制粘贴相似逻辑或把临时兼容判断散落到多个文件。
- 禁止新增无调用方或只有想象中复用价值的公共工具。
- 禁止在一个步骤中同时做大重构和行为变化。
- 禁止为了测试通过隐藏错误或吞掉异常。
