# `stream-server/src` 程序架构

本目录是 RV1106 板端进程 `aipc` 的业务源码。程序把“视频生产”和“视频分发”分开：producer 负责得到类型化 H.264，distribution service 只消费 H.264，不关心输入来自板载 MIPI 还是 USB UVC。

## 目录导航

| 路径 | 职责 |
| --- | --- |
| [`main.cpp`](./main.cpp) | 组装配置、创建服务、连接 producer 与 consumer、运行主事件循环和编排 shutdown。 |
| [`http.cpp`](./http.cpp)、[`http.h`](./http.h) | REST 路由和 handler；调用 `MediaManager` 或 `StreamManager` 完成控制。 |
| [`common/`](./common/README.md) | Asio 事件循环、日志、RKMPI 媒体资源包装和 H.264 Annex-B 解析。 |
| [`httpserver/`](./httpserver/README.md) | 对 cpp-httplib 的通用封装，不包含业务 API。 |
| [`media_producer/`](./media_producer/README.md) | 采集、解码/转换、H.264 编码、producer 生命周期和消费者注册。 |
| [`media_distribution/`](./media_distribution/README.md) | RTSP、WebRTC、WebSocket Preview、MP4 的所有权和分发服务。 |

## 总体依赖方向

```text
main.cpp / HttpApi
  |                     control
  +----> MediaManager ------------> IMediaProducer
  |                                  |-- SimpleIPCProducer
  |                                  `-- UvcH264Producer
  |
  `----> StreamManager ------------> distribution services
                                      |-- RtspService
                                      |-- WebRTCService
                                      |-- WsPreviewServer
                                      `-- FileService

producer -- EncodedStreamPtr (H.264) --> registered consumers
```

必须保持的边界：

- `MediaManager` 是 producer 的生命周期和冷切换入口。
- `StreamManager` 是所有 distribution service 的唯一所有权边界。
- `main.cpp` 负责把 service callback 注册到 producer；底层 producer 不依赖 HTTP 或 distribution。
- `http.cpp` 只负责协议解析、路由和 handler，不拥有媒体硬件资源。
- 所有网络/录制模块消费相同的 `EncodedStreamPtr`，不区分 SimpleIPC 与 UVC 来源。

## 主视频数据流

### SimpleIPC

```text
MIPI sensor / ISP
  -> RKMPI VI -> VPSS -> VENC H.264
  -> EncodedStreamDispatcher
  -> RTSP / WebRTC / WebSocket Preview / File
```

### USB UVC 双目相机

```text
USB UVC MJPEG side-by-side frame
  -> V4L2 mmap + owned UvcFrame
  -> latest-frame mailbox (capacity 1)
  -> RKMPI VDEC (YUV422P)
  -> RGA (NV12)
  -> RKMPI VENC (H.264)
  -> EncodedStreamDispatcher
  -> RTSP / WebRTC / WebSocket Preview / File
```

双目帧在 producer 中保持左右水平拼接，板端不拆左右目。

## 控制流

```text
Web UI / HTTP client
  -> HttpServer
  -> HttpApi handler
     |-- producer mode / resolution -> MediaManager
     |-- RTSP / WebRTC / record      -> StreamManager child service
     `-- status                      -> aggregate current state
```

模式和分辨率切换是冷切换：停止并销毁旧 producer，创建新 producer，重新注册保存的 consumer，再按切换前状态启动。切换期间会短暂停流。

## 启动顺序

`main()` 的关键顺序如下：

1. 初始化日志、信号处理，并停止板端默认 `rkipc`。
2. 解析命令行和环境变量，构造 `StreamConfig`、`HttpApiConfig` 和 `ProducerConfig`。
3. `CreateStreamManager()` 创建需要的 distribution service。
4. 初始化并启动 `HttpApi`。
5. 初始化 `MediaManager` 和选定 producer。
6. 将 RTSP、WebSocket Preview、File、WebRTC callback 注册到 producer。
7. 启动 producer，再调用 `StreamManager::Start()` 启动配置为自动启动的 service。
8. 主线程进入 `IoContext::Run()`。

consumer 必须在 producer 启动前注册；dispatcher 运行时拒绝修改 consumer 列表。

## 线程模型

| 执行上下文 | 主要工作 |
| --- | --- |
| 主线程 / `IoContext` | Asio task 和通过 `AsyncIO` 投递的网络 consumer callback。 |
| cpp-httplib server thread | HTTP 监听和 handler。 |
| VENC fetch thread | `EncodedStreamDispatcher` 获取 H.264、复制原生 VENC pack、分派 consumer。 |
| UVC capture thread | V4L2 DQBUF、复制 MJPEG、先 QBUF、再提交 latest frame。 |
| UVC processing thread | VDEC、RGA、VENC 输入；容量 1 mailbox 满时丢旧帧保最新帧。 |
| 第三方网络内部线程 | RTSP/libdatachannel/WebSocket 的协议处理，具体由对应库管理。 |

当前实现中，只有 `StreamConsumerType::AsyncIO` 会通过 `asio::post` 离开 fetch thread；`Direct` 和 `Queued` 都直接调用 callback，`queue_size` 尚未生效。尤其要避免在非 `AsyncIO` consumer 中加入长耗时操作。

## H.264 所有权

RKMPI 要求 `RK_MPI_VENC_GetStream` 与 `RK_MPI_VENC_ReleaseStream` 在取流线程配对。`common/media_buffer.h` 因此在 fetch thread 中复制当前有效 pack、立即释放原生 VENC stream，再以独立 MB 包装副本形成 `EncodedStreamPtr`。异步 consumer 只共享副本，不跨线程释放原生 VENC 资源。

## 关闭顺序

关闭顺序用于防止 producer 已释放后仍有异步任务持有媒体资源：

1. signal handler 停止 `IoContext`。
2. `MediaManager::Stop()` 停止采集、处理和 VENC fetch。
3. producer 停止后清空 consumer。
4. `IoContext::Drain()` 处理/释放已投递任务。
5. `MediaManager::Deinit()` 释放 VENC、VDEC、RGA、VI/VPSS 等资源。
6. 停止 HTTP，销毁 `StreamManager` 及各 distribution service。
7. 关闭日志系统。

## 常用修改入口

- 新增 producer：实现 `IMediaProducer`，在 `MediaManager` 工厂路径中创建，保持输出为 H.264 `EncodedStreamPtr`。
- 新增 distribution service：由 `StreamManager` 独占持有，在 `main.cpp` 启动 producer 前注册 consumer。
- 新增 HTTP API：在 `HttpApi::SetupRoutes()` 对应路由组绑定 method/path，将业务逻辑放入命名 handler。
- 修改 H.264 解析：复用 [`common/h264_nal_parser.h`](./common/h264_nal_parser.h)，不要另写 start-code parser。
- 修改图像 buffer：使用 RKMPI CAL API 计算 size、stride 和 virtual size，不能假设紧密排列。

## 验证

修改本目录的 C++、头文件或 CMake 后至少执行：

```bash
git diff --check
cmake --build stream-server/build/Debug
```

涉及安装产物时追加 `cmake --install stream-server/build/Debug`；板端媒体行为还需按对应子模块 README 做人工验收。
