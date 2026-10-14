# reCamera Motion Detection (SG200X)

基于 Seeed reCamera（SG200X）平台的轻量级运动检测系统：RTSP 视频输出、帧同步处理、SEI 元数据封装、ROI 区域检测、JPEG 抓拍缓冲、Webhook 事件推送与 HTTP 控制接口。

区域绘制与告警消费由 [reCamera 的 Home Assistant 集成](https://github.com/yyling0101-a11y/recamera-ha)完成；仓库自带的 `windows_ui` Tk 客户端走的是同一套 API。

本项目适用于嵌入式安防、工业检测、边缘 AI 视频分析等场景。

---

## 一、系统特性

- 🎥 RTSP 实时视频流输出（rtsp_demo）
- 🧠 轻量级运动检测（motion_detector）
- 📐 多 ROI 区域管理（roi_manager）
- 📡 HTTP API 控制接口（http_server）
- 📸 JPEG 抓拍环形缓冲（snapshot_store）
- 🔔 带抓拍图的 Webhook 事件推送（notifier）
- ⏱ 帧同步机制（frame_sync）
- 🧾 SEI 元数据封装（frame_sei）
- ⚙ JSON 配置系统（motion_config）
- 🧩 单头 JSON 解析（mini_json）
- 🐳 一条命令交叉编译与部署（`scripts/build.sh`、`scripts/deploy.sh`）

---

## 二、项目结构

```bash
.
├── CMakeLists.txt
├── README.md
├── README - CH.md
├── deploy/
│   └── rootfs/
│       ├── etc/init.d/S96recamera-motion        # 开机服务
│       └── opt/recamera-motion/recamera-motion  # 启动脚本（环境变量、LD_LIBRARY_PATH）
├── scripts/
│   ├── build.sh                                 # riscv64 musl 交叉编译
│   └── deploy.sh                                # 通过 SSH 安装 / 更新 / 卸载
├── main/
│   ├── main.cpp                  # 程序入口
│   ├── motion_detector.*         # 运动检测核心 + 事件回调
│   ├── motion_config.*           # 配置（区域、灵敏度、推送）
│   ├── snapshot_store.hpp        # 保存事件帧的 JPEG 环形缓冲
│   ├── notifier.*                # Webhook 推送线程（重试 + base64 图片）
│   ├── http_server.*             # HTTP 控制接口（多线程、CORS）
│   ├── roi_manager.h             # ROI 管理（多边形区域）
│   ├── roi.h                     # ROI 数据结构定义
│   ├── frame_sync.*              # 多帧同步逻辑
│   ├── frame_sei.*               # SEI 元数据封装
│   ├── rtsp_demo.*               # RTSP 推流模块
│   ├── mini_json.hpp             # 轻量 JSON 解析
│   └── default_motion_roi.json   # 默认 ROI 配置
└── windows_ui/                   # 可选的 Tk 桌面客户端
    ├── README.md
    ├── api_client.py
    ├── app.py
    ├── requirements.txt
    ├── run_windows.bat
    ├── sample_config.json
    ├── status_poller.py
    ├── video_canvas.py
    └── video_worker.py
```

---

## 三、系统架构

```text
Camera Frame (VI)
    ↓
VPSS ──► VENC H.264 ──► rtsp_demo ──► RTSP :8554/live0 ──► NVR / Home Assistant
   │
   ├──► VENC JPEG ──► snapshot_store（环形缓冲，16 帧）
   │                        ▲
   └──► frame_sync ──► motion_detector（320x180 灰度差分 + ROI 掩膜）
                            │   motion_start / motion_end
                            ├──► frame_sei   （元数据写进 H.264 流）
                            ├──► http_server （:8080 状态 / 配置 / 抓拍 / 事件）
                            └──► notifier ──► HTTP POST webhook ──► Home Assistant
```

---

## 四、 模块说明

---

### 1 main.cpp

系统启动入口：

* 从 `MOTION_CONFIG_PATH` 加载已持久化的配置
* 拉起 VI/VPSS，并打开两路编码通道 —— H.264 1920x1080@30 用于 RTSP，
  JPEG 1280x720@10 用于抓拍
* 启动 RTSP 服务（`:8554/live0`）与 HTTP 服务（`:8080`）
* 把降采样后的 Y 分量送进 `motion_detector`
* 把每一帧编码好的 JPEG 写入 `snapshot_store`
* 把检测事件分发到事件环形缓冲与 `notifier`

---

### 2 motion_detector

运动检测核心模块。它在降采样后的 Y（亮度）平面上工作，默认 320x180，
全部是整数运算 —— 不依赖 OpenCV，也不占用 NPU。

每一帧的处理流程：

1. **ROI 掩膜** —— 把 `include` 多边形栅格化进有效掩膜，再从中减去 `exclude`
   多边形。掩膜之外的像素永远不参与统计。
2. **逐像素背景模型** —— `|当前值 - 背景值| >= pixel_threshold` 即判为前景。
   背景是滑动平均：稳定像素用 `background_alpha` 更新，运动像素用慢得多的
   `foreground_alpha` 更新，因此停着的车不会一直触发，走动的人则会持续触发。
3. **3x3 多数滤波** —— 只有 9 邻域中至少 4 个是前景，该像素才保留，用于滤掉传感器噪声。
4. **连通域分析** —— BFS 泛洪填充，得到运动像素总数、最大连通块面积及其外接框。
5. **去抖** —— `min_motion_pixels` / `min_blob_pixels` 决定单帧是否成立；
   连续 `trigger_frames` 帧成立才发 `motion_start`，连续 `clear_frames` 帧不成立
   才发 `motion_end`。
6. **事件** —— 每次状态跳变都会写入事件环形缓冲（由 `/api/events` 提供），
   同时交给已注册的回调，`notifier` 再把它变成 webhook 推送。

启动后的前 `warmup_frames` 帧，以及 `POST /api/background/reset` 之后的前
`warmup_frames` 帧，只用来建立背景模型，不会产生告警。

---

### 3 roi_manager

负责多边形区域管理：

* 支持多个 ROI
* 支持动态更新
* 判断点是否在区域内

```cpp
cv::pointPolygonTest(polygon, point, false)
```

---

### 4 roi.h

定义 ROI 基础结构：

```cpp
struct ROI {
    int id;
    std::vector<cv::Point> points;
};
```

---

### 5 frame_sync

用于解决多源帧不同步问题：

* RTSP / camera frame 对齐
* timestamp 校正
* buffer queue 管理

---

### 6 frame_sei

将检测结果嵌入视频流：

* motion 状态
* ROI 命中信息
* 时间戳

用于：

👉 NVR / AI 设备解析视频附加信息

---

### 7 rtsp_demo

负责视频输出：

* H264/H265 编码流输出
* 推送到 RTSP server
* 可叠加检测结果 overlay

---

### 8 http_server

多线程 HTTP 控制接口，监听 8080 端口（`MOTION_HTTP_PORT` 可改）。

* 每个连接独立线程处理，慢客户端不会拖住检测。
* 始终返回 CORS 头，浏览器页面（包括 Home Assistant 面板）可以直接调用设备。
* 每个路由同时支持 JSON body 和等价的查询参数。

完整路由见 [五、HTTP API](#五-http-api)。

---

### 9 motion_config

管理运行参数：

* 检测灵敏度（像素阈值、最小运动像素、最小连通块）
* 去抖参数（触发帧数 / 恢复帧数 / 预热帧数）
* 背景模型更新速度
* ROI 区域 —— `include` 触发告警，`exclude` 是白名单
* Webhook 推送目标

配置以 JSON 持久化在 `MOTION_CONFIG_PATH`（默认 `/userdata/motion_roi.json`），
重启后区域不丢。区域坐标一律是 **归一化 0–1**，因此与分辨率无关。

```json
{
  "version": 1,
  "enabled": true,
  "pixel_threshold": 2,
  "min_motion_pixels": 1,
  "min_blob_pixels": 1,
  "trigger_frames": 1,
  "clear_frames": 3,
  "warmup_frames": 3,
  "background_alpha": 0.04,
  "foreground_alpha": 0.002,
  "zones": [
    {
      "id": "z1",
      "name": "door",
      "type": "include",
      "points": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]]
    }
  ],
  "notify": {
    "enabled": true,
    "url": "http://homeassistant.local:8123/api/webhook/recamera-<entry_id>",
    "device_name": "reCamera",
    "events": ["motion_start", "motion_end"],
    "include_image": true,
    "image_max_bytes": 524288,
    "timeout_ms": 3000,
    "retries": 2
  }
}
```

---

### 10 mini_json

用于嵌入式环境：

* 无外部依赖
* 解析 ROI / config JSON

---

### 11 default_motion_roi.json

默认 ROI 配置文件

用于：

* 初始区域加载
* 无 UI 情况下启动系统

---

### 12 snapshot_store

纯头文件的 JPEG 环形缓冲（`main/snapshot_store.hpp`）。

* 保存最近 N 帧编码好的 JPEG（1280x720，默认 16 帧）及其采集时间戳。
* 允许客户端在事件发生**之后**再取回当时那一帧，所以每条告警都能带上触发瞬间的图片。
* 支持按 `event_id` 或按绝对毫秒时间戳查询。

---

### 13 notifier

后台 webhook 推送线程（`main/notifier.*`）。

* 把 `motion_start` / `motion_end` / `test` 事件序列化成 JSON。
* `include_image` 打开时，从 `snapshot_store` 取出对应 JPEG 以 base64 附上，
  受 `image_max_bytes` 限制。
* 独立线程 + 有界队列 + 单次超时与重试，接收端慢或不可达都不会拖住检测和视频管线。
* 发送统计通过 `GET /api/notify` 暴露。

---

## 五、 HTTP API

基础地址 `http://<设备IP>:8080`。除 `/api/snapshot.jpg` 外全部返回 JSON。
每个路由都会正确响应 `OPTIONS` 并带上 CORS 头。

| 方法 | 路径 | 作用 |
| --- | --- | --- |
| GET | `/api/health` | 能力探测：抓拍通道尺寸/帧率、RTSP 地址、推送状态 |
| GET | `/api/version` | 程序名、版本、API schema 版本 |
| GET | `/api/status` | 实时状态：运动标志、像素统计、外接框、预热、事件序号 |
| GET | `/api/config` | 当前区域与灵敏度 |
| POST | `/api/config` | 覆盖区域与灵敏度，并落盘持久化 |
| POST | `/api/background/reset` | 重建背景模型（挪动机位后用） |
| GET | `/api/snapshot.jpg` | 当前 JPEG 帧；`?event_id=N` 或 `?at=<unix_ms>` 取缓冲中的历史帧 |
| GET | `/api/events` | 事件列表；`?after_id=N&limit=M` 用于增量轮询 |
| GET | `/api/frames` | 分析缓冲的逐帧诊断数据 |
| GET | `/api/notify` | 推送目标与发送统计 |
| POST | `/api/notify` | 修改推送目标（HA 集成会自动下发） |
| POST | `/api/notify/test` | 立即推送一条 `test` 事件 |

### 状态

```http
GET /api/status
```

```json
{
  "motion": true,
  "event_id": 4,
  "frame_id": "20269",
  "configured": true,
  "enabled": true,
  "warming_up": false,
  "motion_pixels": 116,
  "largest_blob_pixels": 85,
  "motion_ratio": 0.0073,
  "active_mask_pixels": 15840,
  "analysis_width": 320,
  "analysis_height": 180,
  "bbox": [0.36, 0.58, 0.20, 0.31],
  "capture_unix_ms": 1791796312080,
  "last_motion_unix_ms": 1791796312140,
  "last_error": ""
}
```

`bbox` 是归一化的 `[x, y, w, h]`。

### 区域

```http
POST /api/config
```

```json
{
  "enabled": true,
  "pixel_threshold": 2,
  "zones": [
    { "id": "z1", "name": "大门口", "type": "include",
      "points": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]] },
    { "id": "z2", "name": "风扇", "type": "exclude",
      "points": [[0.2, 0.2], [0.3, 0.2], [0.3, 0.3]] }
  ]
}
```

没传的字段保持原值。坐标是归一化 0–1。

### Webhook 推送

`notify.enabled` 为 true 时，设备会把每条事件 POST 到 `notify.url`：

```json
{
  "source": "recamera_motion_detection",
  "schema": 2,
  "event": "motion_start",
  "event_id": 12,
  "device_ip": "192.168.42.1",
  "snapshot_url": "http://192.168.42.1:8080/api/snapshot.jpg?event_id=12",
  "unix_ms": 1791796312140,
  "capture_unix_ms": 1791796312080,
  "frame_id": "20269",
  "motion_pixels": 4297,
  "largest_blob_pixels": 3344,
  "motion_ratio": 0.1166,
  "analysis_width": 320,
  "analysis_height": 180,
  "bbox": [0.606, 0.439, 0.294, 0.461],
  "image": {
    "encoding": "base64",
    "content_type": "image/jpeg",
    "width": 1280,
    "height": 720,
    "capture_unix_ms": 1791796312080,
    "bytes": 42897,
    "data": "<base64 jpeg>"
  }
}
```

以下情况不会带 `image` 字段：`include_image` 为 false、对应帧已被环形缓冲淘汰、
或编码后的 JPEG 超过 `image_max_bytes`。

---

## 六、 编译与部署

交叉编译需要三棵目录树，路径不一致时用环境变量覆盖：

| 变量 | 默认值 | 提供什么 |
| --- | --- | --- |
| `SSCMA_EXAMPLE_PATH` | `$HOME/work/sscma-example-sg200x` | `cmake/` 与 `components/` 构建胶水 |
| `SG200X_SDK_PATH` | `$HOME/work/sdk_dl/sg2002_recamera_emmc` | `cvi_mpi`、`cvi_rtsp` 头文件与库 |
| `HOST_TOOLS_PATH` | `$HOME/work/host-tools` | `riscv64-unknown-linux-musl` 工具链 |

```bash
./scripts/build.sh        # 交叉编译 → <sscma>/solutions/motion_detection/build/motion_detection
./scripts/deploy.sh       # 安装到 /opt/recamera-motion 并注册开机服务
```

`deploy.sh` 参数：

```bash
./scripts/deploy.sh --host root@192.168.4.5   # 换个地址
./scripts/deploy.sh --no-service              # 只拷文件，不重启服务
./scripts/deploy.sh --uninstall               # 全部卸载，并恢复原厂 recamera-onvif
```

脚本使用 SSH 密钥认证，先执行一次 `ssh-copy-id root@192.168.42.1`。

> `deploy.sh` 还会把 SDK 里普通的 `libcvi_rtsp.so` 装到 `/opt/recamera-motion/lib`，
> 启动脚本会把该目录放在 `LD_LIBRARY_PATH` 最前面。固件自带的
> `/usr/local/lib/recamera-onvif/libcvi_rtsp.so` 是厂商改版，对任何 RTSP `DESCRIBE`
> 都回 `401` Digest 认证，绝大多数客户端（包括 Home Assistant）都拉不到流。

---

## 七、 运行方式

以开机服务的形式安装：

```bash
/etc/init.d/S96recamera-motion start|stop|restart|status
tail -f /var/log/recamera-motion.log
```

摄像头管线是**单占用**的，所以该服务启动前会先停掉 `recamera-onvif` 与 `sscma-node`。
不要 `kill -9` 这个进程 —— 硬杀会把 VPSS 管线搞挂；请用 `restart`，实在不行重启设备。

手动运行：

```bash
/opt/recamera-motion/recamera-motion          # 会设置 LD_LIBRARY_PATH 与默认值
MOTION_HTTP_PORT=8080 MOTION_CONFIG_PATH=/userdata/motion_roi.json /opt/recamera-motion/recamera-motion
```

验证：

```bash
curl http://192.168.42.1:8080/api/health
curl http://192.168.42.1:8080/api/status
ffprobe rtsp://192.168.42.1:8554/live0        # H.264 1920x1080@30
```

---

## 八、 Home Assistant

区域绘制、实时画面、通知与抓拍归档由配套集成负责：
[reCamera for Home Assistant](https://github.com/yyling0101-a11y/recamera-ha)。
它通过上面的 HTTP API 与本程序通信，告警走 webhook 推送；设备无法访问 HA 时自动退化为轮询。

仓库自带的 `windows_ui` 客户端仍然可以用同一套 API。

---

## 九、 数据流说明

```text
VI sensor
    ↓
VPSS
    ├──► VENC H.264 ──► rtsp_demo ──► RTSP :8554 ──► NVR / Home Assistant
    └──► VENC JPEG ──► snapshot_store ──► /api/snapshot.jpg  与  webhook 附图
    ↓
frame_sync（时间戳 / 帧对齐）
    ↓
motion_detector（背景模型 + ROI 掩膜）
    ↓
motion_start / motion_end
    ├──► frame_sei  （元数据嵌进 H.264 流）
    ├──► 事件环形缓冲（/api/events、/api/status）
    └──► notifier   （HTTP POST webhook ──► Home Assistant）
```
