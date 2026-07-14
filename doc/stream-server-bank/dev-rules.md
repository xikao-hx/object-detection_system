# 长期开发规则

## 使用方式
- 每个实施步骤开始前阅读本文档。
- 只记录长期有效规则，不记录一次性进度。

## 通用原则
- 采用手术刀式修改，只触碰当前步骤必须修改的文件。
- 优先沿用现有目录职责和 helper 模式。
- 不提前实现后续步骤。
- 不做无关重构。
- 只有多个模块已经稳定需要同一逻辑时，才新增抽象。

## Stream Server 规则
- 运行时 stream consumer 必须在 producer start 前注册；运行中禁止新增 consumer。
- HTTP endpoint path 和 JSON 字段必须保持兼容，除非另有单独的兼容性方案。
- `StreamManager` 是 service 所有权边界。
- 硬件/RKMPI setup helper 不应依赖 HTTP 或 service manager 层。
- 录制时长限制应关闭录制并记录原因日志，不能只打印 warning。
- 共享 H.264 Annex-B 解析逻辑必须复用 `common/h264_nal_parser.h`。
- HTTP 路由注册只做 path 到 handler 的绑定，业务逻辑放入命名 handler。

## 禁止事项
- common parser 步骤完成后，不允许再复制 H.264 解析逻辑。
- 不新增 `void* user_data` 回调路径。
- 不在 `StreamManager` 之外保留全局 service 单例访问路径。
