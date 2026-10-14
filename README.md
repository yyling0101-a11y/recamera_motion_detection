# reCamera Motion Detection (SG200X)

A lightweight motion detection system for the Seeed reCamera (SG200X) platform: RTSP
video output, frame synchronisation, SEI metadata, ROI region detection, JPEG snapshot
buffering, webhook event push and an HTTP control interface.

Regions are drawn and alerts are consumed by the
[reCamera Home Assistant integration](https://github.com/yyling0101-a11y/recamera-ha);
the bundled `windows_ui` Tk client works against the same API.

Suitable for embedded security, industrial inspection, edge video analytics and similar
scenarios.

---

## I. System Features

- 🎥 RTSP Real-time Video Stream Output (rtsp_demo)
- 🧠 Lightweight Motion Detection (motion_detector)
- 📐 Multi-ROI Region Management (roi_manager)
- 📡 HTTP API Control Interface (http_server)
- 📸 JPEG Snapshot Ring Buffer (snapshot_store)
- 🔔 Webhook Event Push with attached frame (notifier)
- ⏱ Frame Synchronization Mechanism (frame_sync)
- 🧾 SEI Metadata Encapsulation (frame_sei)
- ⚙ JSON Configuration System (motion_config)
- 🧩 Single-Header JSON Parsing (mini_json)
- 🐳 One-command cross build and OTA deploy (`scripts/build.sh`, `scripts/deploy.sh`)

Regions are edited and alerts are consumed by the
[reCamera Home Assistant integration](https://github.com/yyling0101-a11y/recamera-ha);
the bundled `windows_ui` client still works against the same HTTP API.

---

## II. Project Structure

```text
.
├── CMakeLists.txt
├── README.md
├── README - CH.md
├── deploy/
│   └── rootfs/
│       ├── etc/init.d/S96recamera-motion        # Boot service
│       └── opt/recamera-motion/recamera-motion  # Launcher (env, LD_LIBRARY_PATH)
├── scripts/
│   ├── build.sh                                 # Cross compile for riscv64 musl
│   └── deploy.sh                                # Install / update / uninstall over SSH
├── main/
│   ├── main.cpp                  # Program entry point
│   ├── motion_detector.*         # Motion detection core + event callback
│   ├── motion_config.*           # Configuration (zones, sensitivity, notify)
│   ├── snapshot_store.hpp        # JPEG ring buffer holding event frames
│   ├── notifier.*                # Webhook push worker (retry + base64 image)
│   ├── http_server.*             # HTTP control interface (threaded, CORS)
│   ├── roi_manager.h             # ROI management (polygon regions)
│   ├── roi.h                     # ROI data structure definition
│   ├── frame_sync.*              # Multi-frame synchronization logic
│   ├── frame_sei.*               # SEI metadata encapsulation
│   ├── rtsp_demo.*               # RTSP streaming module
│   ├── mini_json.hpp             # Lightweight JSON parsing
│   └── default_motion_roi.json   # Default ROI configuration
└── windows_ui/                   # Optional Tk desktop client
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

## III. System Architecture

```text
Camera Frame (VI)
    ↓
VPSS ──► VENC H.264 ──► rtsp_demo ──► RTSP :8554/live0 ──► NVR / Home Assistant
   │
   ├──► VENC JPEG ──► snapshot_store (ring buffer, 16 frames)
   │                        ▲
   └──► frame_sync ──► motion_detector (320x180 grey diff + ROI mask)
                            │   motion_start / motion_end
                            ├──► frame_sei   (metadata inside the H.264 stream)
                            ├──► http_server (:8080 status / config / snapshot / events)
                            └──► notifier ──► HTTP POST webhook ──► Home Assistant
```

---

## IV. Module Description

---

### 1 main.cpp

System start-up entry point:

* Loads the persisted configuration from `MOTION_CONFIG_PATH`
* Brings up VI/VPSS and opens two encoder channels — H.264 1920x1080@30 for RTSP and
  JPEG 1280x720@10 for snapshots
* Starts the RTSP server (`:8554/live0`) and the HTTP server (`:8080`)
* Feeds a downscaled Y plane into `motion_detector`
* Publishes every encoded JPEG into `snapshot_store`
* Routes detector events to the event ring and to `notifier`

---

### 2 motion_detector

Core detection module. It works on a downscaled Y (luma) plane, 320x180 by default, and
is pure integer arithmetic — no OpenCV, no NPU.

Per frame:

1. **ROI mask** — `include` polygons are rasterised into an active mask, `exclude`
   polygons are subtracted from it. Masked-out pixels can never contribute.
2. **Per-pixel background model** — `|current - background| >= pixel_threshold` marks a
   foreground pixel. The background is a running average updated with
   `background_alpha` on stable pixels and the much slower `foreground_alpha` on moving
   ones, so a parked car stops triggering while a walking person keeps doing so.
3. **3x3 majority filter** — a pixel survives only if at least 4 of its 9 neighbours are
   foreground, which removes sensor noise.
4. **Connected components** — a BFS flood fill yields the total motion pixel count, the
   largest blob size and its bounding box.
5. **Debounce** — `min_motion_pixels` / `min_blob_pixels` gate a single frame;
   `trigger_frames` consecutive positive frames raise `motion_start`, `clear_frames`
   consecutive negative frames raise `motion_end`.
6. **Events** — every transition is appended to a ring buffer (served by `/api/events`)
   and handed to the registered callback, which `notifier` turns into a webhook push.

The first `warmup_frames` after start-up, and after `POST /api/background/reset`, only
build the background model and never alert.

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

Threaded HTTP control interface on port 8080 (`MOTION_HTTP_PORT`).

* Every connection is served on its own thread, so a slow client cannot block detection.
* CORS headers are always sent, letting a browser page — including the Home Assistant
  panel — call the device directly.
* Routes accept both a JSON body and equivalent query parameters.

Full route list: [V. HTTP API](#v-http-api).

---

### 9 motion_config

Manages runtime parameters:

* Detection sensitivity (pixel threshold, minimum motion pixels, minimum blob size)
* Debounce (trigger / clear / warm-up frame counts)
* Background model update rates
* ROI zones — `include` triggers, `exclude` is a whitelist
* Webhook notify target

Persisted as JSON at `MOTION_CONFIG_PATH` (default `/userdata/motion_roi.json`), so
regions survive a reboot. Zone coordinates are **normalised 0–1**, which keeps them
valid across resolutions.

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

### 12 snapshot_store

Header-only JPEG ring buffer (`main/snapshot_store.hpp`).

* Keeps the last N encoded JPEG frames (1280x720, default 16) with capture timestamps.
* Lets a client fetch the frame belonging to an event *after* the fact, so every alert
  can carry the picture of the moment it fired.
* Lookup by `event_id` or by an absolute millisecond timestamp.

---

### 13 notifier

Background webhook worker (`main/notifier.*`).

* Serialises `motion_start` / `motion_end` / `test` events to JSON.
* Attaches the matching JPEG from `snapshot_store` as base64 when `include_image` is on,
  capped by `image_max_bytes`.
* Runs on its own thread with a bounded queue, per-event timeout and retries, so a slow
  or unreachable receiver can never stall detection or the video pipeline.
* Exposes send statistics through `GET /api/notify`.

---

## V. HTTP API

Base URL `http://<device-ip>:8080`. All responses are JSON except `/api/snapshot.jpg`.
Every route also answers `OPTIONS` with the right CORS headers.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/health` | Capability probe: snapshot channel size/fps, RTSP URL, notify state |
| GET | `/api/version` | Program name, version, API schema level |
| GET | `/api/status` | Live state: motion flag, pixel counts, bbox, warm-up, event id |
| GET | `/api/config` | Current zones and sensitivity |
| POST | `/api/config` | Replace zones and sensitivity, persisted to disk |
| POST | `/api/background/reset` | Rebuild the background model (after moving the camera) |
| GET | `/api/snapshot.jpg` | Current JPEG frame; `?event_id=N` or `?at=<unix_ms>` for a buffered one |
| GET | `/api/events` | Event list; `?after_id=N&limit=M` for incremental polling |
| GET | `/api/frames` | Per-frame diagnostic ring of the analysis buffer |
| GET | `/api/notify` | Push target plus send statistics |
| POST | `/api/notify` | Change the push target (the HA integration writes this automatically) |
| POST | `/api/notify/test` | Push a `test` event immediately |

### Status

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

`bbox` is normalised `[x, y, w, h]`.

### Regions

```http
POST /api/config
```

```json
{
  "enabled": true,
  "pixel_threshold": 2,
  "zones": [
    { "id": "z1", "name": "door", "type": "include",
      "points": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]] },
    { "id": "z2", "name": "fan", "type": "exclude",
      "points": [[0.2, 0.2], [0.3, 0.2], [0.3, 0.3]] }
  ]
}
```

Omitted keys keep their current value. Coordinates are normalised 0–1.

### Webhook push

When `notify.enabled` is true the device POSTs each event to `notify.url`:

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

`image` is omitted when `include_image` is false, the frame is no longer buffered, or
the encoded JPEG exceeds `image_max_bytes`.

---

## VI. Build and Deploy

Cross compilation needs three trees; override them with environment variables if your
layout differs:

| Variable | Default | Provides |
| --- | --- | --- |
| `SSCMA_EXAMPLE_PATH` | `$HOME/work/sscma-example-sg200x` | `cmake/` and `components/` build glue |
| `SG200X_SDK_PATH` | `$HOME/work/sdk_dl/sg2002_recamera_emmc` | `cvi_mpi`, `cvi_rtsp` headers and libs |
| `HOST_TOOLS_PATH` | `$HOME/work/host-tools` | `riscv64-unknown-linux-musl` toolchain |

```bash
./scripts/build.sh        # cross compile -> <sscma>/solutions/motion_detection/build/motion_detection
./scripts/deploy.sh       # install to /opt/recamera-motion and enable the boot service
```

`deploy.sh` options:

```bash
./scripts/deploy.sh --host root@192.168.4.5   # a different address
./scripts/deploy.sh --no-service              # copy files without restarting
./scripts/deploy.sh --uninstall               # remove everything, restart stock recamera-onvif
```

It uses SSH key authentication, so run `ssh-copy-id root@192.168.42.1` once.

> `deploy.sh` also installs the plain SDK `libcvi_rtsp.so` into `/opt/recamera-motion/lib`
> and the launcher puts that directory first in `LD_LIBRARY_PATH`. The firmware copy in
> `/usr/local/lib/recamera-onvif` is a vendor variant that answers every RTSP `DESCRIBE`
> with `401` Digest auth, which most clients (including Home Assistant) cannot handle.

---

## VII. Running

Installed as a boot service:

```bash
/etc/init.d/S96recamera-motion start|stop|restart|status
tail -f /var/log/recamera-motion.log
```

The camera pipeline has a single owner, so the service stops `recamera-onvif` and
`sscma-node` before starting. Never `kill -9` the process — a hard kill wedges the VPSS
pipeline; use `restart`, or reboot the device.

To run it by hand:

```bash
/opt/recamera-motion/recamera-motion          # sets LD_LIBRARY_PATH and defaults
MOTION_HTTP_PORT=8080 MOTION_CONFIG_PATH=/userdata/motion_roi.json /opt/recamera-motion/recamera-motion
```

Verify:

```bash
curl http://192.168.42.1:8080/api/health
curl http://192.168.42.1:8080/api/status
ffprobe rtsp://192.168.42.1:8554/live0        # H.264 1920x1080@30
```

---

## VIII. Home Assistant

Regions, live view, notifications and the snapshot archive are handled by the companion
integration: [reCamera for Home Assistant](https://github.com/yyling0101-a11y/recamera-ha).
It talks to this program over the HTTP API above and receives alerts through the webhook
push, falling back to polling when the device cannot reach Home Assistant.

The bundled `windows_ui` client keeps working against the same API.

---

## IX. Data Flow

```text
VI sensor
    ↓
VPSS
    ├──► VENC H.264 ──► rtsp_demo ──► RTSP :8554 ──► NVR / Home Assistant
    └──► VENC JPEG ──► snapshot_store ──► /api/snapshot.jpg  &  webhook image
    ↓
frame_sync (timestamp / frame alignment)
    ↓
motion_detector (background model + ROI mask)
    ↓
motion_start / motion_end
    ├──► frame_sei  (metadata embedded in the H.264 stream)
    ├──► event ring (/api/events, /api/status)
    └──► notifier   (HTTP POST webhook ──► Home Assistant)
```
