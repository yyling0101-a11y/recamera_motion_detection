# reCamera 运动检测（SG200X）

本项目面向 Seeed reCamera SG200X：在 320×180 的亮度画面上检测指定区域的运动，通过 HTTP 提供状态和 JPEG 抓拍，可输出 H.264 RTSP 视频，并通过 Webhook 推送运动事件。检测本身不依赖 OpenCV 或 NPU。

[reCamera Home Assistant 集成](https://github.com/yyling0101-a11y/recamera-ha)提供区域绘制、实时画面和通知；仓库的 `windows_ui/` 还提供可选的 PySide6 桌面标注工具。英文说明见 [README.md](README.md)。

## 快速开始

编译需要 SG200X SDK 和 RISC-V musl 交叉编译工具链。下表中的目录准备好后运行：

```bash
./scripts/build.sh
./scripts/deploy.sh                          # 安装到 root@192.168.42.1 并重启服务
curl http://192.168.42.1:8080/api/health
ffprobe rtsp://192.168.42.1:8554/live0
```

| 环境变量 | 默认值 | 用途 |
| --- | --- | --- |
| `SSCMA_EXAMPLE_PATH` | `$HOME/work/sscma-example-sg200x` | CMake 构建框架与组件 |
| `SG200X_SDK_PATH` | `$HOME/work/sdk_dl/sg2002_recamera_emmc` | 设备 SDK 头文件与库 |
| `HOST_TOOLS_PATH` | `$HOME/work/host-tools` | `riscv64-unknown-linux-musl` 工具链 |

`build.sh` 会把工程复制到 `$SSCMA_EXAMPLE_PATH/solutions/motion_detection`，可执行文件生成在该目录的 `build/` 下。`deploy.sh` 使用 SSH 密钥认证；需要时先执行 `ssh-copy-id root@192.168.42.1`。可以用 `--host root@<ip>` 指定其他设备，`--no-service` 只复制不重启，`--uninstall` 卸载并重启原厂 `recamera-onvif` 服务。部署脚本还会将 SDK 的 `libcvi_rtsp.so` 安装到程序旁边。

安装后的服务为 `/etc/init.d/S96recamera-motion`，查看状态和日志：

```bash
/etc/init.d/S96recamera-motion status
tail -f /var/log/recamera-motion.log
```

摄像头管线只能由一个进程占用。服务启动前会停止 `recamera-onvif` 和 `sscma-node`；需要重启时请使用服务的 `restart` 命令，不要强制杀进程。

## 设备端代码结构

| 文件 | 职责 |
| --- | --- |
| `main/main.cpp` | 读取启动参数，连接检测器、通知器、视频通道和 HTTP 服务 |
| `main/video_stack.{hpp,cpp}` | 配置运动检测、JPEG 和可选 H.264 通道；接收帧并管理 RTSP 启动 |
| `main/app_api.{hpp,cpp}` | 处理应用层 API 路由和配置持久化 |
| `main/event_payload.{hpp,cpp}` | 生成 Webhook 事件 JSON，编码可选的 JPEG 附图 |
| `main/motion_detector.{hpp,cpp}` | 背景模型、ROI 掩膜、运动状态切换及事件/帧历史 |
| `main/motion_config.{hpp,cpp}` | 配置默认值、校验、JSON 解析和文件存储 |
| `main/snapshot_store.hpp` | 保存最近 JPEG 帧的有界环形缓冲，用于实时和事件抓拍 |
| `main/http_server.{hpp,cpp}` | HTTP 连接、请求解析和 CORS |
| `main/notifier.{hpp,cpp}` | 异步 Webhook 队列、重试与发送统计 |
| `main/rtsp_demo.{h,cpp}`、`frame_sync.*`、`frame_sei.*` | RTSP 输出与帧元数据支持 |
| `main/mini_json.hpp` | 嵌入式 JSON 解析 |
| `main/default_motion_roi.json` | 配置示例；程序不会自动加载此文件 |

新增设备端 `.cpp` 文件时，需要将它加入 `main/CMakeLists.txt` 的 `SRCS`。

运动检测通道处理 320×180、10 fps 的 NV21 亮度平面。`include` 多边形圈定检测范围，`exclude` 多边形从范围中排除像素。检测器建立滑动背景模型、过滤噪声、统计连通区域，并在满足触发/恢复帧数后发出 `motion_start` 或 `motion_end`。JPEG 抓拍单独缓存，默认 1280×720、10 fps；启用 RTSP 时，H.264 通道为 1920×1080、30 fps。RTSP 代码包含帧同步和 SEI 支持。

## 配置

程序启动时读取 `MOTION_CONFIG_PATH`（默认 `/userdata/motion_roi.json`）。文件不存在或无效时使用内置默认值。`main/default_motion_roi.json` 只是示例，不是自动加载的备用配置。运动检测和通知设置可通过 HTTP API 修改；RTSP 设置会保存，但需要重启服务才生效。

```json
{
  "version": 1,
  "enabled": true,
  "pixel_threshold": 20,
  "min_motion_pixels": 120,
  "min_blob_pixels": 40,
  "trigger_frames": 3,
  "clear_frames": 5,
  "warmup_frames": 20,
  "background_alpha": 0.04,
  "foreground_alpha": 0.002,
  "zones": [
    {
      "id": "door",
      "name": "大门",
      "type": "include",
      "points": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]]
    }
  ],
  "rtsp": {"enabled": true, "port": 8554, "path": "/live0"},
  "notify": {
    "enabled": false,
    "url": "",
    "device_name": "reCamera",
    "events": ["motion_start", "motion_end"],
    "include_image": true,
    "image_max_bytes": 524288,
    "timeout_ms": 3000,
    "retries": 2
  }
}
```

区域坐标归一化到 0–1，每个区域至少需要三个顶点。`include` 区域参与检测，`exclude` 区域被忽略。启动或重建背景后的前 `warmup_frames` 帧只建立背景模型，不产生告警。

`rtsp` 默认启用，端口为 8554，路径为 `/live0`。路径只能有一个以斜杠开头的段，斜杠后最多 63 个字符，仅允许 ASCII 字母、数字、`_` 和 `-`。保存设置后，如果与当前运行的监听配置不同，响应会返回 `rtsp.restart_required`。设置 `rtsp.enabled: false` 并重启服务，可以关闭编码通道和监听器。

其他启动环境变量：`MOTION_HTTP_PORT`（默认 8080）、`MOTION_DEVICE_IP`（覆盖对外公布的 IP）、`MOTION_SNAPSHOT_WIDTH`（1280）、`MOTION_SNAPSHOT_HEIGHT`（720）、`MOTION_SNAPSHOT_FPS`（10；设为 0 可关闭抓拍）。

当前版本没有 MQTT 客户端。`/api/health` 返回 `mqtt.supported: false`，提交包含 `mqtt` 块的配置会被拒绝。

## HTTP API

默认地址为 `http://<设备IP>:8080`。除 JPEG 抓拍外，响应均为 JSON。服务端会发送 CORS 头，并处理 `OPTIONS` 请求。

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/api/health` | 连通性、RTSP 和抓拍实际状态、通知状态、MQTT 能力 |
| GET | `/api/version` | 程序版本与 API 版本 |
| GET | `/api/status` | 当前检测状态和最近一次统计 |
| GET / POST | `/api/config` | 读取或保存运动、区域、通知及 RTSP 配置 |
| POST | `/api/background/reset` | 重建背景模型 |
| GET | `/api/snapshot.jpg` | 最新 JPEG，或用 `?event_id=N` / `?at=<unix_ms>` 查缓冲帧 |
| GET | `/api/events` | 事件历史；`?after_id=N&limit=M` 可增量读取 |
| GET | `/api/frames` | 最近分析帧的诊断数据 |
| GET / POST | `/api/notify` | 查看发送统计或更新 Webhook 地址 |
| POST | `/api/notify/test` | 将测试通知加入发送队列 |

`POST /api/config` 中未传的运动参数会恢复为默认值；未传 `notify` 或 `rtsp` 块时，这两个块会保留当前值。因此，更新运动参数时应提交需要保留的完整设置。只改通知地址可使用 `POST /api/notify`。

Webhook JSON 包含事件类型、事件 ID、采集时间、运动像素统计、归一化外接框和抓拍 URL；启用附图且有可用帧时还会带 base64 JPEG。通知器通过有界后台队列发送，不会阻塞帧处理。发送和失败计数可在 `GET /api/notify` 查看。

## Home Assistant 与桌面标注工具

使用 [Home Assistant 集成](https://github.com/yyling0101-a11y/recamera-ha) 可以绘制区域、查看 RTSP 画面、接收 Webhook 告警和浏览抓拍；也可以直接调用上面的设备 API。

可选的 Windows 标注工具使用 PySide6。在 `windows_ui/` 下运行：

```powershell
python -m pip install -r requirements.txt
python app.py --rtsp rtsp://192.168.42.1:8554/live0 --api http://192.168.42.1:8080
```

也可以双击 `run_windows.bat`，再在界面中修改地址。连接 RTSP 后绘制“检测区域”（Include），需要时在持续运动的位置绘制“忽略区域”（Exclude），最后上传到设备。左键增加顶点；右键、Backspace 或 Z 撤销最后一个顶点；Esc 取消当前多边形。工具保存的是归一化坐标。


## Home Assistant application bundle / HA 应用配置

`HA_config/recamera-app.json` declares the app metadata and supported connections.
`HA_config/recamera-panel.js` provides the sandboxed app page in the reCamera
Home Assistant integration. Keep both files in ZIP uploads; the device-side
program and Solution `.deb` installation are separate.
