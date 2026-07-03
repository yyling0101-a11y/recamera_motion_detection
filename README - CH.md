# reCamera Motion Detection (SG200X)

基于 Seeed reCamera（SG200X）平台的轻量级运动检测系统，集成 RTSP 视频输出、帧同步处理、SEI 元数据封装、ROI 区域检测与 HTTP 控制接口。

本项目适用于嵌入式安防、工业检测、边缘 AI 视频分析等场景。

---

## 一、系统特性

- 🎥 RTSP 实时视频流输出（rtsp_demo）
- 🧠 轻量级运动检测（motion_detector）
- 📐 多 ROI 区域管理（roi_manager）
- 📡 HTTP API 控制接口（http_server）
- ⏱ 帧同步机制（frame_sync）
- 🧾 SEI 元数据封装（frame_sei）
- ⚙ JSON 配置系统（motion_config）
- 🧩 单头 JSON 解析（mini_json）

---

## 二、项目结构

```bash
.
├── CMakeLists.txt
├── README.md
└── main/
    ├── main.cpp                  # 程序入口
    ├── motion_detector.*        # 运动检测核心
    ├── motion_config.*          # 配置管理
    ├── roi_manager.h            # ROI 管理（多边形区域）
    ├── roi.h                    # ROI 数据结构定义
    ├── frame_sync.*            # 多帧同步逻辑
    ├── frame_sei.*             # SEI 元数据封装
    ├── rtsp_demo.*             # RTSP 推流模块
    ├── http_server.*           # HTTP 控制接口
    ├── mini_json.hpp           # 轻量 JSON 解析
    └── default_motion_roi.json # 默认 ROI 配置

├── CMakeLists.txt
├── README-ch.md
├── README.md
├── main
│   ├── main.cpp                  # 程序入口
│   ├── motion_detector.*        # 运动检测核心
│   ├── motion_config.*          # 配置管理
│   ├── roi_manager.h            # ROI 管理（多边形区域）
│   ├── roi.h                    # ROI 数据结构定义
│   ├── frame_sync.*            # 多帧同步逻辑
│   ├── frame_sei.*             # SEI 元数据封装
│   ├── rtsp_demo.*             # RTSP 推流模块
│   ├── http_server.*           # HTTP 控制接口
│   ├── mini_json.hpp           # 轻量 JSON 解析
│   └── default_motion_roi.json # 默认 ROI 配置
└── windows_ui
    ├── README.md
    ├── api_client.py
    ├── app.py
    ├── requirements.txt
    ├── run_windows.bat
    ├── sample_config.json
    ├── status_poller.py
    ├── video_canvas.py
    └── video_worker.py

````

---

## 三、系统架构

```text
Camera Frame
    ↓
frame_sync (时间戳/帧对齐)
    ↓
motion_detector (运动检测)
    ↓
roi_manager (区域过滤)
    ↓
frame_sei (元数据封装)
    ↓
rtsp_demo (RTSP 推流)
    ↓
Client / NVR / UI
```

---

## 四、 模块说明

---

### 1 main.cpp

系统启动入口，负责：

* 初始化配置
* 启动 HTTP Server
* 启动 RTSP 推流
* 初始化 ROI / Motion 模块

---

### 2 motion_detector

运动检测核心模块。

功能：

* 帧差检测 / 简化背景建模
* 输出 motion mask
* 支持 ROI 过滤前的原始检测结果

核心逻辑示意：

```cpp
absdiff(frame_t, frame_t-1, diff);
threshold(diff, binary, motion_threshold);
```

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

提供外部控制能力：

* ROI 配置
* motion 参数调整
* 状态查询

示例接口：

```http
POST /api/roi/set
GET  /api/motion/status
POST /api/config/update
```

---

### 9 motion_config

管理运行参数：

* motion threshold
* ROI enable
* frame sync settings

支持 JSON 配置加载：

```json
{
  "motion_threshold": 25,
  "min_area": 500,
  "roi_enabled": true
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

## 五、 HTTP API

---

### 1 获取状态

```http
GET /api/motion/status
```

返回：

```json
{
  "motion": true,
  "timestamp": 12345678
}
```

---

### 2 设置 ROI

```http
POST /api/roi/set
```

```json
{
  "id": 1,
  "points": [[100,200],[400,200],[300,400]]
}
```

---

### 3 更新配置

```http
POST /api/config/update
```

---

## 六、 编译方式

```bash
mkdir build
cd build
cmake ..
make -j4
```

---

## 运行方式

```bash
./motion_detection
```

---

## 数据流说明

```text
RTSP / Camera Input
        ↓
Frame Sync
        ↓
Motion Detection
        ↓
ROI Filter
        ↓
SEI Embed
        ↓
RTSP Output
        ↓
NVR / Client
```