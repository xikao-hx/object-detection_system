# 实施计划

## 总体原则
- 每次只实施一个小步骤。
- 每一步记录修改文件、结构规则和验证方式。
- 当前步骤未经 review/测试确认前，不进入下一步。

## Step 1：安全性与配置修复

### 目标
- 在 stream manager shutdown 时停止 WebSocket 预览。
- 让 VENC 码率/帧率使用 `SimpleIPCConfig`。
- 移除 `MediaManager` const 加锁点里的 `const_cast`。

### 涉及文件
- `stream-sever/src/media_distribution/stream_manager.cpp`
- `stream-sever/src/media_producer/simple_ipc/mpi_config.h`
- `stream-sever/src/media_producer/simple_ipc/simple_ipc_producer.cpp`
- `stream-sever/src/media_producer/media_manager.cpp`
- `memory-bank/*`

### 结构验收点
- 不改变 HTTP API。
- 暂不改变回调模型。
- 暂不清理 service 单例死代码。

### 验证
```bash
cmake --build stream-sever/build/Debug
```

## Step 2：删除 service 全局单例死代码

### 目标
- 删除 RTSP、file、WebRTC、WebSocket preview service 中未使用的全局单例 helper。
- 保持 `StreamManager` 作为唯一 service 访问入口。

### 验证
```bash
cmake --build stream-sever/build/Debug
```

## Step 3：类型化 Stream Consumer 与运行时注册保护

### 目标
- 用类型化成员回调替换 `static StreamConsumer(EncodedStreamPtr, void*)`。
- 在 producer start 前完成 consumer 注册。
- dispatcher 运行中拒绝注册/清理 consumer。

### 验证
```bash
cmake --build stream-sever/build/Debug
```

## Step 4：共享 H.264 NAL Parser

### 目标
- 新增 `common/h264_nal_parser.h`。
- 替换 file saver、WebSocket preview、WebRTC 中重复的 SPS/PPS/关键帧解析逻辑。

### 验证
```bash
cmake --build stream-sever/build/Debug
```

## Step 5：录制时长自动停止

### 目标
- 让 `maxDurationSec` 关闭录制并记录原因日志。
- 保持 MP4 close/write 顺序正确。

### 验证
```bash
cmake --build stream-sever/build/Debug
```

## Step 6：拆分 HTTP 路由

### 目标
- 将 `HttpApi::SetupRoutes()` 拆为路由分组和命名 handler。
- 保持所有 path 和 JSON 字段兼容。

### 验证
```bash
cmake --build stream-sever/build/Debug
```

## Step 7：双目拼接流默认适配

### 目标
- 新增 SimpleIPC `1280x480` 双目左右拼接分辨率预设。
- 将板端默认输出切换为 `1280x480 @ 30fps`，供 PyQt 客户端拆分左右目并做双目算法。
- 同步 WebRTC 与 MP4 录制的默认视频尺寸。
- 扩展 HTTP 分辨率状态/切换值，保持原有 path、message 和 JSON 字段兼容。

### 涉及文件
- `stream-sever/src/media_producer/simple_ipc/simple_ipc_config.h`
- `stream-sever/src/media_producer/simple_ipc/simple_ipc_producer.h`
- `stream-sever/src/media_producer/media_manager.h`
- `stream-sever/src/main.cpp`
- `stream-sever/src/http.cpp`
- `memory-bank/*`

### 结构验收点
- 不新增双路 stream service，不改变 consumer 注册规则。
- 不把双目算法放入板端推流链路。
- HTTP path、message、既有 JSON 字段保持兼容，仅增加可选分辨率值。

### 验证
```bash
git diff --check
cmake --build stream-sever/build/Debug
```
