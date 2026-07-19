# 技术栈

## 选择结果
- 语言：沿用 `stream-server/src` 中已有的 C++17 风格项目约定。
- 构建：沿用 `stream-server/src` 下已有 CMake 文件。
- HTTP：沿用现有 `httplib` 封装。
- 媒体：沿用现有 RKMPI、libdatachannel、FFmpeg 集成。
- 文档：使用 `memory-bank/` 下的 markdown 文件。

## 选择理由
- 复用当前技术栈可以避免引入新的板端依赖。
- 大多数改动都是围绕所有权、回调、解析 helper 和路由 handler 的局部重构。
- 只有在 helper 足够小且不会制造新依赖环时，才接受 header-only helper。

## 备选方案与取舍
- 新增路由框架：不采用。API 兼容性和嵌入式构建风险比框架功能更重要。
- 用锁支持动态消费者注册：暂不采用。当前确认的运行规则是启动后禁止新增消费者。
- 围绕 `shared_ptr` 重写 service 生命周期：早期步骤不采用。`StreamManager` 已经具备清晰的 `unique_ptr` 所有权。

## 工程约束
- 保持 HTTP 响应兼容。
- 避免 common helper 与 service 模块之间出现双向依赖。
- 不引入网络/包管理依赖。
- 能用本地构建命令验证时就验证；若受工具链限制无法验证，必须说明限制。
