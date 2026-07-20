# HTTP Server 基础封装

`httpserver/` 是对 cpp-httplib 的薄封装，只提供 server 配置、路由注册、静态目录和生命周期。AIPC 的业务 endpoint 和 handler 位于上一级 [`http.cpp`](../http.cpp)。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| [`http_server.h`](./http_server.h) | `HttpServerConfig`、请求/响应类型别名、handler 类型和公开接口。 |
| [`http_server.cpp`](./http_server.cpp) | cpp-httplib server 创建、线程池、路由转发、静态挂载和 server thread。 |

## 调用关系

```text
main.cpp
  -> HttpApi::Init
     -> HttpServer::Init
     -> HttpApi::SetupRoutes
        -> HttpServer::{Get,Post,Put,Delete}
  -> HttpApi::Start
     -> HttpServer::Start (independent listen thread)
```

`HttpServer` 不认识 `MediaManager`、`StreamManager`、producer 或任一 distribution service。业务 handler 通过 `HttpApi` 捕获 `this`，再访问对应 manager/service。

## 生命周期

1. `Init(config)` 创建 httplib server，配置线程池和可选静态目录。
2. 业务层注册所有路由。
3. `Start()` 创建监听线程并立即返回。
4. `Stop()` 调用 httplib stop 并 join server thread。
5. 析构函数再次调用 `Stop()`，保证 RAII 清理。

路由应在 `Start()` 前注册。不要在此模块加入具体 `/api/...` path 或媒体业务逻辑。

## 配置

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `host` | `0.0.0.0` | 监听地址。 |
| `port` | `8080` | HTTP 端口。 |
| `static_dir` | 空 | 静态文件目录；AIPC 传入安装目录旁的 `www`。 |
| `static_mount` | `/` | 静态文件挂载点。 |
| `thread_pool_size` | `4` | cpp-httplib task queue 的工作线程数。 |

## AIPC 业务路由分组

上层 `HttpApi` 当前注册以下路由族：

- 系统：`GET /api/status`
- RTSP：`GET /api/rtsp/status`、`POST /api/rtsp/start|stop`
- WebRTC：status、start/stop、offer/answer、ICE 和 candidates
- 录制：`GET /api/record/status`、`POST /api/record/start|stop`
- Producer：status 和模式切换
- Pipeline：status 和分辨率冷切换
- AI/model：保留的兼容接口和模型文件管理

新增 API 时，在 `HttpApi` 对应的 `Register*Routes()` 中只绑定 method/path，业务逻辑放入命名 `Handle*` 方法；默认保持既有 status code、message 和 JSON 字段兼容。

## 并发注意事项

handler 运行在 httplib 线程池，不在全局 Asio `IoContext`。调用 manager 的切换或 service 启停时必须使用它们现有的同步/锁边界；不要从 handler 直接操作 producer 内部硬件对象。
