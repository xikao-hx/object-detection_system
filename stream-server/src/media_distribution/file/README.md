# File / MP4 录制模块

`media_distribution/file` 把 producer 输出的 H.264/H.265 编码数据封装为 MP4。它不重新编码视频，也不包含 JPEG 拍照功能。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| [`file_service.h/.cpp`](./file_service.h) | `FileService`：录制控制门面、状态查询和 consumer 入口。 |
| [`file_saver.h/.cpp`](./file_saver.h) | `Mp4Recorder`：FFmpeg muxer、SPS/PPS extradata、时间戳、限制和文件关闭。 |

## 数据流

```text
EncodedStreamPtr
  -> FileService::OnEncodedStream
  -> if recording: Mp4Recorder::WriteFrame
  -> parse SPS/PPS and write MP4 packets
  -> close finalized MP4 file
```

`Mp4Recorder` 使用 mutex 串行保护 FFmpeg context、header 状态和文件关闭。只有处于 `kRecording` 时才写入 frame；达到时长或文件大小限制时必须关闭录制并记录原因。

## 当前线程事实

`main.cpp` 将 File consumer 注册为 `StreamConsumerType::Queued`，但当前 `EncodedStreamDispatcher` 尚未实现 per-consumer queue：`Queued` 与 `Direct` 一样在 VENC fetch thread 直接调用。`queue_size=10` 也尚未生效。

`FileService::Start()` 当前只设置 `running_`，不会创建文件线程；`OnEncodedStream()` 直接调用 `Mp4Recorder::WriteFrame()`。因此旧文档中“独立文件线程、不阻塞取流”的描述不适用于当前实现。若要隔离磁盘 I/O，应先在 dispatcher 或 FileService 中设计有界队列、丢帧/背压和 shutdown drain，而不是只修改枚举值。

## 生命周期与控制

```text
construct FileService -> construct Mp4Recorder
POST /api/record/start -> StartRecording -> create output context/file
incoming H.264         -> WriteFrame
POST /api/record/stop  -> StopRecording -> write trailer/close file
StreamManager::Stop    -> ensure recording stopped
```

HTTP 接口：

- `GET /api/record/status`
- `POST /api/record/start`
- `POST /api/record/stop`

默认输出目录由 `AIPC_RECORD_DIR` 指定，未设置时为 `/root/record`。

## H.264 与 MP4

输入是 Annex-B 编码数据。recorder 从流中提取 SPS/PPS 构造 codec extradata，写 header 后再写 packet。NAL 解析应复用 `common/h264_nal_parser.h`。输入的 `EncodedStreamPtr` 由调用方共享持有，recorder 不手动释放其中的 MB。

## 配置和限制

`Mp4RecordConfig` 包含输出目录、文件名前缀、宽高、fps、GOP、codec、最大时长和最大大小。宽高/fps 在创建 `StreamManager` 时确定；producer 运行期冷切换分辨率不会自动重建 recorder 配置。

## 构建依赖

模块从 `BUILDROOT_SYSROOT` 检查并链接 FFmpeg `avformat`、`avcodec`、`avutil`、`swresample`、`swscale` 及其系统依赖。缺少头文件或库时 CMake 配置会明确失败。
