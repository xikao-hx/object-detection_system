# 代码设计

## 当前功能
- 功能名称：Stream Server review 重构
- 所属实施步骤：Step 1-6
- 关联需求：按 code review 文档完成安全性、回调、解析、录制和 HTTP 路由重构

## 成功标准
- 行为：`StreamManager::Stop()` 会停止 WebSocket 预览。
- 行为：VENC 码率和帧率来自 `SimpleIPCConfig`。
- 行为：运行中禁止新增或清理 stream consumer。
- 行为：`maxDurationSec` 到期后自动停止录制并记录日志。
- 结构：service 不再暴露全局单例 helper，不再使用 `void* user_data` 回调。
- 结构：H.264 Annex-B 解析集中在 `common/h264_nal_parser.h`。
- 结构：HTTP 路由注册与业务 handler 分离，API 响应保持兼容。
- 验证：每步执行 `cmake --build stream-sever/build/Debug` 并记录结果。

## 现有代码观察
- `StreamManager::Start()` 会启动 WebSocket 预览，但 `Stop()` 没有停止它。
- `MediaManager::CreateProducerInstance()` 已经把 `ProducerConfig` 复制到 `SimpleIPCConfig`。
- `SimpleIPCProducer::InitMpi()` 调用 `venc_init()` 时没有传入码率/帧率。
- `MediaManager::mutex_` 已经声明为 `mutable`。
- service 实例实际由 `StreamManager` 持有，旧全局 service helper 是死代码。
- H.264 start code / SPS / PPS / keyframe 判断分散在 file、wspreview、webrtc 模块中。
- `HttpApi::SetupRoutes()` 同时承担路由注册和业务逻辑，难以局部验证。

## 修改文件
- `stream-sever/src/media_distribution/stream_manager.cpp`：补充 WebSocket 预览停止逻辑。
- `stream-sever/src/media_producer/simple_ipc/mpi_config.h`：扩展 `venc_init()` 参数。
- `stream-sever/src/media_producer/simple_ipc/simple_ipc_producer.cpp`：把 config 值传给 VENC 初始化。
- `stream-sever/src/media_producer/media_manager.cpp`：移除 `const_cast`。
- `stream-sever/src/media_distribution/*/*service*`：删除全局 service helper 和 `void*` 回调。
- `stream-sever/src/main.cpp`：注册类型化 service 成员回调，调整 shutdown 清理顺序。
- `stream-sever/src/common/h264_nal_parser.h`：新增共享 H.264 parser。
- `stream-sever/src/media_distribution/file/file_saver.cpp`：复用 parser，并实现录制超时自动停止。
- `stream-sever/src/media_distribution/wspreview/ws_preview.cpp`：复用 parser。
- `stream-sever/src/media_distribution/webrtc/webrtc.cpp`：复用 parser。
- `stream-sever/src/http.h` / `stream-sever/src/http.cpp`：拆分路由分组和 handler。

## 禁止修改范围
- 不改变 HTTP path、message 和 JSON 字段。
- 不引入新的外部依赖。
- 不支持运行时动态新增 consumer。
- 不把 RKMPI/common helper 反向依赖到 HTTP 或 service manager 层。

## 职责划分
- `MediaManager`：持有通用 producer 配置。
- `SimpleIPCProducer`：将通用配置映射到 SimpleIPC/RKMPI 初始化。
- `mpi_config.h`：只包含 RKMPI setup 细节。
- `StreamManager`：停止它持有的 services。
- distribution services：只提供自身生命周期、状态查询和类型化帧处理入口。
- `common/h264_nal_parser.h`：只负责 H.264 Annex-B 字节流解析。
- `HttpApi`：路由注册和 handler 分离，handler 只调用公开 manager/service API。

## 依赖方向
- 允许：`SimpleIPCProducer` -> `simple_ipc/mpi_config.h`。
- 允许：`StreamManager` -> distribution service public APIs。
- 禁止：common/RKMPI helper 依赖 HTTP 或 distribution managers。

## 方案取舍
- 采用：给 `venc_init()` 增加显式的码率/帧率参数。
- 不采用：把整个 `SimpleIPCConfig` 传入 `mpi_config.h`。
- 原因：`mpi_config.h` 应保持专注于基础 RKMPI setup，避免依赖更高层 config 类型。
- 采用：运行中拒绝 consumer 变更。
- 不采用：给 consumer 列表加锁支持动态增删。
- 原因：当前产品约束明确禁止运行中新增 consumer，拒绝策略更简单且运行期开销更低。

## 风险与约束
- RKMPI 可能期望码率单位为 kbps；现有硬编码 `10 * 1024` 和 config 命名都使用 kbps，因此保持单位不变。
- fps 至少钳制到 1，避免无效分母/分子值。

## 验证方式
```bash
cmake --build stream-sever/build/Debug
```

## 步骤结果
- 已按设计实现。
- 实际成功验证命令：
```bash
cmake --build stream-sever/build/Debug
```
- `stream-sever/build` 不是已配置的 CMake 构建目录；`stream-sever/build/Debug` 包含 `CMakeCache.txt`。

## 后续步骤结果
- Step 2：删除 service 全局单例死代码，保留 `StreamManager` 唯一所有权。
- Step 3：删除 `void* user_data` 回调路径，运行中拒绝 consumer 注册/清理。
- Step 4：新增 `common/h264_nal_parser.h` 并替换重复 H.264 解析。
- Step 5：`maxDurationSec` 到期自动停止录制并记录日志。
- Step 6：拆分 `HttpApi::SetupRoutes()` 为路由分组和命名 handler，保持 API 兼容。
- Step 7：新增 `R_STEREO_1280X480` 双目拼接分辨率预设，并将 SimpleIPC 默认输出、WebRTC 参数和 MP4 录制参数同步为 `1280x480 @ 30fps`。

## 最终验证方式
```bash
git diff --check
cmake --build stream-sever/build/Debug
```
