# 设计文档

## 需求理解
- 目标用户是嵌入式 stream server 的维护者。
- 系统负责相机采集、H.264 编码、RTSP/WebRTC/WebSocket 分发、MP4 录制和 HTTP 控制。
- 重构必须保留现有 API 行为，并尊重嵌入式运行时假设。

## 已确认需求
- 使用 memory-bank 工作流推进重构。
- 禁止运行时新增消费者。
- 录制时长到期后必须自动停止录制，并记录提示日志。
- HTTP API 必须保持兼容。
- 当前双目适配先由板端推送 `1280x480` 左右拼接图像，PyQt 客户端负责拆分左右目和后续双目算法。

## 功能设计

### 生命周期与配置修复
- 输入：现有 `StreamConfig`、`ProducerConfig`、`SimpleIPCConfig`。
- 处理：将码率/帧率传入 VENC 初始化；shutdown 时停止每个由 manager 持有的 service。
- 输出：除配置开始真实生效、shutdown 更完整之外，用户可见行为不变。

### Stream Consumer 注册
- 输入：由 `StreamManager` 持有的 service 对象指针。
- 处理：注册类型化 lambda，lambda 直接调用成员函数。
- 输出：service 回调路径不再出现 `void*` 转换。
- 边界：dispatcher 启动后动态注册消费者时，拒绝并记录 warning。

### H.264 NAL 解析
- 输入：Annex-B 格式 H.264 字节流。
- 处理：一个 common helper 定位 NAL 单元，并提取 SPS/PPS/关键帧状态。
- 输出：各现有模块继续收到它们原本需要的数据格式。

### HTTP 路由重构
- 输入：现有 HTTP 请求。
- 处理：路由注册只做分发，具体逻辑委托给命名 handler。
- 输出：JSON schema 和 HTTP path 保持兼容。

### 录制时长处理
- 输入：编码帧和 recorder 配置。
- 处理：达到最大录制时长时，通过 recorder/service 路径关闭录制并记录日志。
- 输出：有效关闭的 MP4 文件和可观察日志。

### 双目拼接流适配
- 输入：双目相机输出的左右拼接帧。
- 处理：SimpleIPC 增加 `1280x480` 预设并作为默认板端输出，WebRTC 与 MP4 录制尺寸跟随该预设。
- 输出：RTSP/WebRTC/WebSocket/MP4 均接收同一路 `1280x480` H.264 拼接流。
- 边界：不在板端拆分左右目，不引入深度、测距或双路分发。

## 关键流程
1. `main.cpp` 创建 `StreamManager`。
2. `MediaManager` 初始化选定 producer。
3. consumer 在 producer start 之前注册。
4. producer 启动 Fetch loop，并向固定 consumer 分发帧。
5. service 在析构前停止，并确保待处理异步回调不会越过资源生命周期。

## 数据与状态
- `MediaManager` 持有 producer 生命周期和已保存的 consumer 注册信息。
- `StreamManager` 是 distribution service 的唯一所有者和访问入口。
- `SimpleIPCConfig` 将码率/帧率带入 RKMPI VENC 初始化。
- MP4 recorder 持有录制状态和时长限制执行逻辑。

## 验收标准
- 每一步都有小范围变更文件集和验证命令。
- 不主动重命名任何 endpoint path 或 JSON key。
- parser 步骤完成后，distribution 模块不再保留重复的 H.264 parser。
