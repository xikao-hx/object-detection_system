# SimpleIPC Producer

`simple_ipc/` 实现板载 MIPI/RKISP 输入的纯硬件采集编码链路，对外实现 `IMediaProducer`。

## 数据流

```text
sensor / ISP
  -> VI device 0, channel 0
  -> bind VI -> VPSS group 0, channel 0
  -> bind VPSS -> VENC channel 0
  -> H.264 EncodedStreamDispatcher
```

VI 使用 DMABUF，VI、VPSS 和 VENC 由 RKMPI 绑定，CPU 不逐帧搬运原始图像。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| [`simple_ipc_config.h`](./simple_ipc_config.h) | 分辨率 preset、宽高/帧率/码率映射和 `SimpleIPCConfig`。 |
| [`simple_ipc_producer.h/.cpp`](./simple_ipc_producer.h) | `IMediaProducer` 实现、MPI 生命周期、binding 和 dispatcher。 |
| [`mpi_config.h`](./mpi_config.h) | VI/VPSS 属性配置与启停 helper。 |

## 初始化与释放

初始化方向：

```text
RK_MPI_SYS_Init
  -> ISP / VI
  -> VPSS
  -> VENC
  -> bind VI-to-VPSS
  -> bind VPSS-to-VENC
```

`Start()` 主要启动 `EncodedStreamDispatcher`；硬件 binding 已在初始化阶段建立。`Stop()` 停止 dispatcher，`Deinit()` 按相反方向解除 binding、停止并销毁 VENC/VPSS/VI/ISP/SYS 资源。

如果新增初始化步骤，必须同时补齐所有部分失败路径和 Deinit 逆序清理。

## 分辨率

当前 preset 为 `1080p`、`720p`、`480p`。preset 到尺寸的唯一映射位于 `simple_ipc_config.h`。分辨率不能在硬件运行期原地修改，`MediaManager::SetResolution(simple_ipc::Resolution)` 会重建 producer。

HTTP 层只解析 preset 字符串，不应复制 VI/VPSS/VENC 尺寸规则。

## 边界

- 本模块只输出 H.264，不负责网络协议或文件封装。
- 不在这里操作 `StreamManager` 或响应 HTTP。
- VI/VPSS 配置 helper 只负责硬件 setup，不承载模式切换。
- VENC 共有参数使用 `rk_venc_config.h`，不要另写一套码率/GOP 配置。
- 当前板端若没有可用 MIPI sensor，初始化失败应保留原始 RKAIQ/RKMPI 错误。
