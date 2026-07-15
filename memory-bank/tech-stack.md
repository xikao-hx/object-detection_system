# 双目 UVC 图像采集技术栈

## 选择结果

- 语言：现有项目一致的 C++17。
- 采集 API：Linux V4L2 ioctl 与 mmap。
- 并发：`std::thread`、`std::atomic`、`std::condition_variable`。
- 性能计时：`std::chrono::steady_clock`，仅做进程内耗时和帧率汇总，不使用 wall clock。
- 构建：现有 CMake 子模块结构和 Luckfox 交叉工具链。
- 日志：复用 `common/logger.h`。
- JPEG 解码与像素转换：工程现有 FFmpeg `libavcodec` + `libswscale`。
- H.264 编码：RV1106 RKMPI VENC，NV12 input MB pool。
- 硬件 MJPEG 能力探测：RKMPI VDEC `VIDEO_MODE_FRAME`、`RK_VIDEO_ID_MJPEG`，请求 `RK_FMT_YUV420SP` 但必须按 decoded frame 的实际 pixel format 验证和保存；该相机首个实测输出为 `RK_FMT_YUV422P`。SDK 的 VDEC 符号来自 `librockit_full.so`，仅 probe 链接并随安装包部署该库。

## 选择理由

- V4L2 是 `uvcvideo` 的原生接口，不需要引入 OpenCV/GStreamer 等板端运行依赖。
- mmap 符合相机开发文档的验证路径，并减少额外系统调用拷贝；仅在回调边界复制一次以保证 buffer 重新入队后的数据所有权安全。
- `uvc_lib` 保持 USB capture 独立；`uvc_h264_lib` 组合 FFmpeg/RKMPI 并输出既有 H.264 类型。

## 备选方案与取舍

- OpenCV `VideoCapture`：未选，增加板端依赖且弱化设备能力校验和 buffer 生命周期控制。
- GStreamer：未选，当前 rootfs 的插件可用性未确认，首阶段不需要完整媒体 pipeline。
- 让原始 `UvcProducer` 直接实现 `IMediaProducer`：未选，原始 MJPEG 与 `EncodedStreamPtr` 不兼容。
- 独立 MJPEG WebSocket/PyQt：用户明确取消。
- `UvcH264Producer` 实现 `IMediaProducer`：采用，只有完成 H.264 编码后才进入现有 consumer。

## 工程约束

- UVC 层不得依赖 HTTP、`MediaManager`、`StreamManager` 或 distribution service。
- 不新增 `void *user_data` 回调，使用类型化 `std::function`。
- 所有 consumer/callback 必须在 producer start 前设置，运行中禁止修改。
- 设备发现不得仅按 `/sys/.../name` 字符串匹配，最终选择必须以 V4L2 capability 和 format 枚举为准。
- `stream-server/src/` 修改后执行 `git diff --check` 和 Debug 交叉构建。
- 性能日志每 100 帧或固定时间汇总一次；禁止逐帧 INFO 日志改变待测链路负载。
- NV12 buffer 尺寸和 stride 必须使用 `RK_MPI_CAL_COMM_GetPicBufferSize/GetHorStride`，不能直接假设 `width*height*3/2` 的无对齐布局。
