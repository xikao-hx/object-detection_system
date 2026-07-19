# Stream Server 代码 Review

> 审查日期：2026-06-16
> 目录：`stream-server/src/`

---

## 总体评价

代码整体质量在嵌入式 C++ 项目中属于上乘。架构设计有清晰的主线：

- **RAII 资源管理**（`media_buffer.h`）用 `shared_ptr` 自定义删除器封装 RKMPI 原生结构体，引用计数归零时自动调用 `ReleaseStream`，零泄漏
- **单线程 Asio 事件模型 + 独立 Fetch/File 线程**（`asio_context.h`）适配 RV1106 单核场景，shutdown 编排通过 `Drain()` 处理了多线程资源释放顺序
- **策略模式 + 冷切换**（`IMediaProducer` / `MediaManager`）接口定义干净，`SwitchMode()` 的销毁-重建-回退流程完整
- **PIMPL 隐藏 MPI 细节**（`SimpleIPCProducer::Impl`）头文件零泄露 RKMPI 结构体

以下问题按严重程度排列。

---

## 🔴 严重问题

### 1. `void* user_data` 回调模式——类型安全黑洞

**所有 Service 层**（`RtspService`、`FileService`、`WebRTCService`、`WsPreviewServer`）统一定义了带 `void*` 的静态回调，然后在主流程中用 lambda 包装一层：

```cpp
// main.cpp — 注册时包一层 lambda
media_manager.RegisterStreamConsumer("rtsp",
    [](EncodedStreamPtr stream) {
        RtspService::StreamConsumer(stream, GetStreamManager()->GetRtspService());
    }, ...);

// rtsp_service.cpp — service 端再转换回来
static void StreamConsumer(EncodedStreamPtr stream, void* user_data) {
    auto* self = static_cast<RtspService*>(user_data);  // 裸 static_cast
    if (self && self->running_) { ... }
}
```

**问题**：lambda 闭包已经捕获了 `GetStreamManager()->GetRtspService()` 的指针，`void*` 层完全多余——让每处调用都多了一次 `static_cast`，放弃了编译期类型检查。四份代码完全重复相同模式，属于架构层面的样板代码。

**影响**：中等（代码臃肿、类型不安全）
**建议**：去掉 `void*`，直接注册 lambda 调用成员函数。`IMediaProducer` 的 `StreamCallback` 类型是 `std::function<void(EncodedStreamPtr)>`，完全可以直接传 `[svc](auto s) { svc->OnStream(s); }`。

---

### 2. 每层模块各自声明了全局单例管理函数——但全是死代码

`rtsp_service.h`、`file_service.h`、`webrtc_service.h`、`ws_preview.h` 各自声明了：

```cpp
XxxService* GetXxxService();
void CreateXxxService(const Config& config);
void DestroyXxxService();
```

每个 .cpp 里还各有一个 `static std::unique_ptr<X> g_xxx` 的全局变量。

**问题**：这些函数在 `main.cpp` 中从未被调用。实际实例全由 `StreamManager` 以成员 `unique_ptr` 持有。这意味着：
- 4 个 `static` 全局变量永远为 `nullptr`，浪费空间
- 新维护者看到 `GetRtspService()` 会误以为可以直接获取全局实例，实际拿到 `nullptr`
- 与 `StreamManager` 构成了两套互相矛盾的"获取 service 实例"的方式

**影响**：中等（死代码、误导维护者）
**建议**：全部删除，只保留 `StreamManager` 的 `GetRtspService()` / `GetFileService()` 等单一访问入口。

---

### 3. `SetupRoutes()` —— 一个 733 行的巨函数

`http.cpp:129-733` 把所有 API 路由和业务逻辑全部内联在 lambda 里。

**问题**：
- 不可单元测试——handler 逻辑被埋没在 lambda 中无法独立验证
- AI 接口、Pipeline 接口、Model 管理、录制控制全部杂糅
- 每个 handler 都在重复获取 `GetStreamManager()` 并判空

**影响**：高（单测阻障、难以并行开发）
**建议**：拆分为 `HandleRtspStatus()`、`HandleRecordStart()`、`HandleWebrtcOffer()` 等成员函数；路由注册只做分发。对 `GetStreamManager()` 的重复空检查可以提取中间件或辅助函数。

---

### 4. NAL 解析代码在 4 个文件中重复实现

| 文件 | 重复的 NAL 逻辑 |
|------|----------------|
| `file_saver.cpp` | `FindNalUnit()`, `SetExtradataFromStream()` |
| `ws_preview.cpp` | `ExtractSpsPps()`, `IsKeyframe()` |
| `webrtc.cpp` | `SendVideoData()` 中的关键帧检测 |
| `media_buffer.h` | `acquire_encoded_frame()` 中的关键帧判断 |

同样的 H.264 start code 搜索、NAL type 判断、SPS/PPS 提取逻辑写了至少三遍。

**影响**：高（维护成本翻倍、bug 会反复修复 N 次）
**建议**：提取为 `common/h264_nal_parser.h`，包含 `FindNextNal()`、`IsKeyframe()`、`ExtractSpsPps()` 等函数。

---

### 5. `venc_init()` 硬编码了码率和 GOP，`SimpleIPCConfig` 的 `bitrate_kbps` 形同虚设

[mpi_config.h:185-191](src/media_producer/simple_ipc/mpi_config.h#L185-L191)

```cpp
stAttr.stRcAttr.stH264Cbr.u32BitRate = 10 * 1024;  // 硬编码 10Mbps
stAttr.stRcAttr.stH264Cbr.u32Gop = 30;
```

而 `SimpleIPCConfig` 传入了 `bitrate_kbps`，`MediaManager::CreateProducerInstance()` 也正确赋值给了 `sipc_config.bitrate_kbps`——但 `venc_init()` 签名里根本没接收码率参数，内部写死了 `10 * 1024`。

**影响**：中（用户配置不生效）
**建议**：`venc_init()` 增加 `bitrate_kbps` 和 `gop` 参数，或直接传入 `SimpleIPCConfig`。

---

### 6. `StreamManager::Stop()` 漏停了 WsPreviewServer

[stream_manager.cpp:110-129](src/media_distribution/stream_manager.cpp#L110-L129)

```cpp
void StreamManager::Stop() {
    if (file_service_ && file_service_->IsRecording()) file_service_->StopRecording();
    if (rtsp_service_) rtsp_service_->Stop();
    if (webrtc_service_) webrtc_service_->Stop();
    // ❌ ws_preview_server_ 被遗漏了
}
```

**影响**：中（shutdown 时序中 WebSocket 连接仍打开，可能导致 `IoContext::Drain()` 仍有悬挂回调）
**建议**：加上 `if (ws_preview_server_) ws_preview_server_->Stop();`

---

### 7. `media_manager.cpp:IsRunning()` 锁的用法可疑

```cpp
bool MediaManager::IsRunning() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(mutex_));
    return producer_ && producer_->IsRunning();
}
```

在 const 成员函数中 `const_cast` 掉 mutex 的 const——说明 `mutex_` 应该声明为 `mutable`。虽然技术上对 `std::mutex` 的 `mutable` 声明是惯用法，但这里用 `const_cast` 绕过也是一种可行写法。更核心的问题是：`producer_->IsRunning()` 调用发生在锁外（`producer_` 本身是线程安全的 `atomic`），这个锁实际上只保护了 `producer_` 指针本身不被并发 reset，语义是**自旋读指针**，可能不必要。

**影响**：低
**建议**：将 `mutex_` 声明为 `mutable`，去掉 `const_cast`。

---

## 🟡 中等问题

### 8. `SimpleStreamDispatcher` 的消费者列表没有线程保护

`FetchLoop` 在独立线程中遍历 `consumers_`，而 `RegisterConsumer()` 和 `ClearConsumers()` 可以被外部线程调用。当前不出事是因为注册都在 `Start()` 前、清理都在 `Stop()` 后，但没有任何机制阻止开发者在运行中添加消费者。

**建议**：给 `consumers_` 加 `mutex`，或者在 `RegisterConsumer()` 中检查 `running_` 状态并拒绝。

### 9. `WebRTCService::IsRunning()` 语义与 RTSP 不一致

```cpp
// webrtc_service.h
bool IsRunning() const { return valid_; }  // valid_ 仅是"已启动"
// rtsp_service.h
bool IsRunning() const { return running_; }  // running_ 是"正在推流"
```

同一套接口在不同模块中语义不同，调用方无法统一信任。

**建议**：统一生命周期状态表示，或改名以示区分。

### 10. `Mp4Recorder::WriteFrame()` 中锁的范围与注释

```cpp
bool Mp4Recorder::WriteFrame(const EncodedStreamPtr& stream) {
    // 前三段逻辑（检查 state、提取数据、关键帧等待）无锁
    // ...
    // header_written_ 在第 409 行无锁写入
    header_written_ = true;

    // 然后在第 414 行才加锁写帧数据
    std::lock_guard<std::mutex> lock(mutex_);
    // ...
}
```

`header_written_` 的写入在锁外，而 `ExtractSpsPps` 等操作也修改了 `format_ctx_` 和 `codec_ctx_`——这些成员在其他地方（`CloseOutputFile`）是锁保护的。此外帧率控制的 `maxDurationSec` 检查（第 456 行）写了一条日志但没做实际停止，是功能缺失。

### 11. `ws_preview_.cpp:bytes_sent_` 统计意义不明确

```cpp
bytes_sent_ += size * active_clients.size();  // clients.size() 个客户端次全部累加
frames_sent_++;                                // 但 frames 只加一次
```

`frames_sent_` 是"处理了多少输入帧"，`bytes_sent_` 是"发送了多少字节（乘了客户端数）"。变量的度量单位不一致，阅读时容易困惑。

**建议**：明确区分 `frames_received` 和 `bytes_wire_sent`。

### 12. HTTP 路由中 `this` 捕获的生命期

`SetupRoutes()` 中部分 lambda `[this]` 捕获了 `HttpApi*`。如果 `HttpApi` 被析构而 httplib::Server 仍在线程中运行，回调访问 `this->stream_config_` 是 UAF。当前代码的 shutdown 顺序正确（先 `Stop()` 后 `reset()`），但缺乏防御性措施。

**建议**：对捕获了 `this` 的 lambda，用 `weak_ptr` + `shared_from_this()` 模式，或在 Server 的 `Stop()` 中清空路由表。

---

## 文件级小问题摘录

| 文件 | 问题 |
|------|------|
| `media_buffer.h:147` | `VENC_STREAM_S stream` 声明在栈上但内部 `pstPack` 指向栈变量，跨函数传递有栈悬挂风险 |
| `file_saver.cpp:456` | `maxDurationSec` 检测只打了日志，没有实际停止录制 |
| `signaling.cpp:85` | `ws_->open(config_.server_url)` 后立即返回 `true`，此时连接可能尚未建立（异步） |
| `webrtc.cpp:848` | `CreateOfferForHttp()` 在锁外设置 `pending_local_sdp_`，回调可能在另一线程中并发写入 |

---

## 架构层面的正面设计（值得保持）

| 设计 | 说明 |
|------|------|
| **RAII 资源管理** | `media_buffer.h` 用 shared_ptr 自定义删除器管理 RKMPI 原生结构体，生命周期自然 | 
| **Drain 编排** | `IoContext::Drain()` 在 shutdown 时确保所有异步回调完成后再释放硬件资源 |
| **冷切换容错** | `SwitchMode()` 在失败时恢复旧模式，保证系统不卡在中间状态 |
| **AsyncIO 分类** | 三种消费者类型（Direct/AsyncIO/Queued）适配了不同延迟需求 |
| **PIMPL** | `SimpleIPCProducer::Impl` 隐藏了 RKMPI 所有结构体依赖 |

---

## 改进优先级建议

```
高优先级（约 2 小时）:
  ├── 1. 抽取 H264NalParser → 消除 4 处重复
  ├── 2. venc_init 添加 bitrate_kbps 参数
  ├── 3. StreamManager::Stop() 补上 ws_preview_server_
  └── 4. 删除各 service 的全局单例死代码

中优先级（约 1 天）:
  ├── 5. 拆解 SetupRoutes() 为成员函数
  ├── 6. 清除 void* user_data 回调模式
  └── 7. SimpleStreamDispatcher consumers_ 加锁

低优先级（可长期跟踪）:
  ├── 8. 统一 Service 生命周期命名
  ├── 9. 明确统计指标语义
  └── 10. 路由 lambda 的生命周期防御
```
