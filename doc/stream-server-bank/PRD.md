# Stream Server 重构 PRD

## 背景
- `doc/stream-sever-code-review.md` 指出了 H.264 解析重复、service 回调类型不安全、全局 service 单例死代码、HTTP 路由巨函数，以及若干生命周期/配置问题。
- stream server 运行在嵌入式 RV1106 目标板上，因此改动必须保守处理 shutdown 顺序、回调生命周期和运行期开销。

## 目标
- 在深入结构重构前，先修复明确的生命周期和配置问题。
- 用类型安全、单一所有权的接口替换重复且不安全的模式。
- 保持现有 HTTP API 响应结构与当前前端兼容。
- 使用 `memory-bank/` 记录设计、实施顺序、进度和架构约束。

## 非目标
- 本次重构不改变前端 API 契约。
- 不引入新的外部运行时依赖。
- 不支持运行时动态注册消费者；消费者必须在推流启动前注册。
- 不在 review 范围之外大幅重写 media producer 或 service 架构。

## 核心功能
- 安全停止所有已启用的流服务，包括 WebSocket 预览。
- VENC 码率/帧率配置真实反映 `ProducerConfig` / `SimpleIPCConfig`。
- 使用类型化 stream consumer 注册方式，移除 `void* user_data`。
- 为重复的 SPS/PPS/关键帧逻辑提供共享 H.264 Annex-B NAL 解析 helper。
- 拆小 HTTP 路由 handler，并保持 JSON 输出兼容。
- 录制时长达到上限后自动停止录制并记录日志原因。

## 用户路径
1. 用户像以前一样启动 stream server。
2. 现有 HTTP/Web UI 控制仍使用相同 endpoint 契约。
3. 各流服务可以安全启动/停止。
4. 到达配置的录制时长后，录制自动停止。

## 验收标准
- 现有构建目标仍可编译。
- 前端使用的 HTTP API 字段保持兼容。
- dispatcher 启动后不能再新增运行时 stream consumer。
- `maxDurationSec` 会触发录制停止，并记录说明性 warning/info 日志。
- shutdown 会停止 RTSP、WebRTC、WebSocket 预览和正在进行的录制。
