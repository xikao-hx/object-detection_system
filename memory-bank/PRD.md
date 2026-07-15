# USB 双目相机图像采集

## 背景

- 当前 `stream-server` 只支持面向 MIPI/RKISP 的 SimpleIPC 链路，不能采集 HBVCAM USB UVC 双目相机。
- 相机输出左右拼接的 MJPEG 图像，设备节点会随插拔变化，不能固定为 `/dev/video0`。

## 目标

- 在 Luckfox Pico Ultra W 上自动发现满足条件的 UVC capture 节点。
- 以 V4L2 mmap 方式稳定采集 `MJPG 1280x480 30fps` 帧。
- 将 MJPEG 解码并通过 RV1106 VENC 转为现有 RTSP 可消费的 H.264。
- 定位并逐步消除软件 MJPEG 转码瓶颈，使 `1280x480@30fps` 输入最终能接近 30fps 实时输出。
- 提供可独立部署到板端的采集诊断程序。

## 非目标

- 当前阶段先以 RTSP 验证完整 H.264 推流，暂不处理 PyQt 推理。
- 当前阶段不在板端拆分左右图；拆分由收到 JPEG 的客户端或算法层完成。
- 不修改设备树、USB 网络配置或现有 HTTP API。

## 核心功能

- 自动发现 `uvcvideo`、Video Capture、Streaming 且支持目标 MJPEG 规格的节点。
- 配置并管理 V4L2 mmap buffer 和 stream 生命周期。
- 在专用采集线程中复制有效帧并调用类型化回调。
- 使用 FFmpeg 解 MJPEG、swscale 转 NV12、RKMPI VENC 编码 H.264。
- 复用现有 RTSP、WebRTC、WebSocket H.264 consumers；RTSP URL 保持 `rtsp://<device-ip>:554/live/0`。
- 通过诊断程序采集指定帧数并保存一张 JPEG 用于人工检查。

## 用户使用路径

1. 用户把诊断程序部署到已枚举 UVC 摄像头的 Luckfox 板端。
2. 程序自动选择设备，或由用户通过 `--device` 指定节点。
3. 用户以 `start_app.sh --mode uvc` 启动完整链路。
4. 用户分别通过 RTSP、WebSocket Preview 和 WebRTC 原有入口确认 `1280x480` 左右拼接图。

## 验收标准

- 无固定节点参数时能避开 rkcif/rkisp/metadata 节点并选择合格 UVC capture 节点。
- 不支持 `MJPG 1280x480` 的节点会在初始化阶段明确失败。
- 能连续采集至少 100 帧，并生成可解码的 `1280x480` JPEG。
- `Stop()`/`Deinit()` 后采集线程、mmap 和 fd 均被释放。
- UVC 模式能持续产生 H.264，并由原 RTSP、WebRTC、WebSocket Preview 路径消费。
- 性能优化阶段必须先用低频分段统计证明瓶颈和丢帧边界，再分步实施采集解耦与硬件解码；每阶段独立板端验收。
