# reCamera Motion Detection (SG200X)

A lightweight motion detection system based on the Seeed reCamera (SG200X) platform, integrating RTSP video output, frame synchronization processing, SEI metadata encapsulation, ROI region detection, and an HTTP control interface.

This project is suitable for embedded security, industrial inspection, edge AI video analysis, and other scenarios.

---

## I. System Features

- 🎥 RTSP Real-time Video Stream Output (rtsp_demo)
- 🧠 Lightweight Motion Detection (motion_detector)
- 📐 Multi-ROI Region Management (roi_manager)
- 📡 HTTP API Control Interface (http_server)
- ⏱ Frame Synchronization Mechanism (frame_sync)
- 🧾 SEI Metadata Encapsulation (frame_sei)
- ⚙ JSON Configuration System (motion_config)
- 🧩 Single-Header JSON Parsing (mini_json)

---

## II. Project Structure

```bash
.
├── CMakeLists.txt
├── README.md
└── main/
    ├── main.cpp                  # Program entry point
    ├── motion_detector.*        # Motion detection core
    ├── motion_config.*          # Configuration management
    ├── roi_manager.h            # ROI management (polygon regions)
    ├── roi.h                    # ROI data structure definition
    ├── frame_sync.*            # Multi-frame synchronization logic
    ├── frame_sei.*             # SEI metadata encapsulation
    ├── rtsp_demo.*             # RTSP streaming module
    ├── http_server.*           # HTTP control interface
    ├── mini_json.hpp           # Lightweight JSON parsing
    └── default_motion_roi.json # Default ROI configuration

├── CMakeLists.txt
├── README-ch.md
├── README.md
├── main
│   ├── main.cpp                  # Program entry point
│   ├── motion_detector.*        # Motion detection core
│   ├── motion_config.*          # Configuration management
│   ├── roi_manager.h            # ROI management (polygon regions)
│   ├── roi.h                    # ROI data structure definition
│   ├── frame_sync.*            # Multi-frame synchronization logic
│   ├── frame_sei.*             # SEI metadata encapsulation
│   ├── rtsp_demo.*             # RTSP streaming module
│   ├── http_server.*           # HTTP control interface
│   ├── mini_json.hpp           # Lightweight JSON parsing
│   └── default_motion_roi.json # Default ROI configuration
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

## III. System Architecture

```text
Camera Frame
    ↓
frame_sync (Timestamp/Frame Alignment)
    ↓
motion_detector (Motion Detection)
    ↓
roi_manager (Region Filtering)
    ↓
frame_sei (Metadata Encapsulation)
    ↓
rtsp_demo (RTSP Streaming)
    ↓
Client / NVR / UI
```

---

## IV. Module Description

---

### 1 main.cpp

System startup entry point, responsible for:

* Initializing configuration
* Starting HTTP Server
* Starting RTSP streaming
* Initializing ROI / Motion modules

---

### 2 motion_detector

Core motion detection module.

Functionality:

* Frame differencing / simplified background modeling
* Outputs motion mask
* Supports raw detection results before ROI filtering

Core logic illustration:

```cpp
absdiff(frame_t, frame_t-1, diff);
threshold(diff, binary, motion_threshold);
```

---

### 3 roi_manager

Responsible for polygon region management:

* Supports multiple ROIs
* Supports dynamic updates
* Determines if a point is within a region

```cpp
cv::pointPolygonTest(polygon, point, false)
```

---

### 4 roi.h

Defines the basic ROI structure:

```cpp
struct ROI {
    int id;
    std::vector<cv::Point> points;
};
```

---

### 5 frame_sync

Used to solve multi-source frame synchronization issues:

* RTSP / camera frame alignment
* Timestamp correction
* Buffer queue management

---

### 6 frame_sei

Embeds detection results into the video stream:

* Motion status
* ROI hit information
* Timestamp

Used for:

👉 NVR / AI device parsing of video auxiliary information

---

### 7 rtsp_demo

Responsible for video output:

* H264/H265 encoded stream output
* Pushing to RTSP server
* Optional overlay of detection results

---

### 8 http_server

Provides external control capabilities:

* ROI configuration
* Motion parameter adjustment
* Status query

Example interfaces:

```http
POST /api/roi/set
GET  /api/motion/status
POST /api/config/update
```

---

### 9 motion_config

Manages runtime parameters:

* Motion threshold
* ROI enable
* Frame sync settings

Supports JSON configuration loading:

```json
{
  "motion_threshold": 25,
  "min_area": 500,
  "roi_enabled": true
}
```

---

### 10 mini_json

For embedded environments:

* No external dependencies
* Parses ROI / config JSON

---

### 11 default_motion_roi.json

Default ROI configuration file

Used for:

* Initial region loading
* Starting the system without a UI

---

## V. HTTP API

---

### 1 Get Status

```http
GET /api/motion/status
```

Returns:

```json
{
  "motion": true,
  "timestamp": 12345678
}
```

---

### 2 Set ROI

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

### 3 Update Configuration

```http
POST /api/config/update
```

---

## VI. Compilation Method

```bash
mkdir build
cd build
cmake ..
make -j4
```

---

## Running Method

```bash
./motion_detection
```

---

## Data Flow Description

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