# reCamera Motion Detection (SG200X)

Motion detection for the Seeed reCamera SG200X. The device analyzes selected regions of a 320×180 luma frame, serves live status and JPEG snapshots over HTTP, can publish an H.264 RTSP stream, and sends motion events to a webhook. No OpenCV or NPU is required for detection.

The [reCamera Home Assistant integration](https://github.com/yyling0101-a11y/recamera-ha) provides region editing, live view, and notifications. An optional PySide6 desktop editor is included in `windows_ui/`. For Chinese instructions, see [README - CH.md](README%20-%20CH.md).

## Quick start

The build uses the SG200X SDK and a RISC-V musl cross compiler. With the paths in the table below available:

```bash
./scripts/build.sh
./scripts/deploy.sh                          # installs to root@192.168.42.1 and restarts the service
curl http://192.168.42.1:8080/api/health
ffprobe rtsp://192.168.42.1:8554/live0
```

| Variable | Default | Purpose |
| --- | --- | --- |
| `SSCMA_EXAMPLE_PATH` | `$HOME/work/sscma-example-sg200x` | CMake build framework and components |
| `SG200X_SDK_PATH` | `$HOME/work/sdk_dl/sg2002_recamera_emmc` | Device SDK headers and libraries |
| `HOST_TOOLS_PATH` | `$HOME/work/host-tools` | `riscv64-unknown-linux-musl` toolchain |

`build.sh` copies the project into `$SSCMA_EXAMPLE_PATH/solutions/motion_detection` and writes the executable to its `build/` directory. `deploy.sh` uses SSH key authentication; run `ssh-copy-id root@192.168.42.1` first if needed. Use `--host root@<ip>` for another device, `--no-service` to copy without restarting, or `--uninstall` to remove the app and restart the stock `recamera-onvif` service. Deployment also installs the SDK `libcvi_rtsp.so` beside the executable.

The installed service is `/etc/init.d/S96recamera-motion`. Check it with:

```bash
/etc/init.d/S96recamera-motion status
tail -f /var/log/recamera-motion.log
```

The camera pipeline has one owner. The service stops `recamera-onvif` and `sscma-node` before starting; use the service's `restart` command rather than force-killing the process.

## Device-side code

| File | Responsibility |
| --- | --- |
| `main/main.cpp` | Read startup settings and connect the detector, notifier, video stack, and HTTP server |
| `main/video_stack.{hpp,cpp}` | Configure motion, JPEG, and optional H.264 channels; collect frames and manage RTSP startup |
| `main/app_api.{hpp,cpp}` | Handle application API routes and persist configuration |
| `main/event_payload.{hpp,cpp}` | Build webhook event JSON and encode optional JPEG data |
| `main/motion_detector.{hpp,cpp}` | Background model, ROI masks, motion transitions, and recent event/frame history |
| `main/motion_config.{hpp,cpp}` | Configuration defaults, validation, JSON parsing, and file storage |
| `main/snapshot_store.hpp` | Bounded ring of encoded JPEG frames for live and event snapshots |
| `main/http_server.{hpp,cpp}` | HTTP transport, request parsing, and CORS |
| `main/notifier.{hpp,cpp}` | Asynchronous webhook queue, retries, and delivery statistics |
| `main/rtsp_demo.{h,cpp}`, `frame_sync.*`, `frame_sei.*` | RTSP output and frame metadata support |
| `main/mini_json.hpp` | Embedded JSON parser |
| `main/default_motion_roi.json` | Example configuration; the runtime does not load it automatically |

Add new device-side `.cpp` files to `main/CMakeLists.txt` under `SRCS`.

The motion channel uses an NV21 luma plane at 320×180 and 10 fps. Include polygons select pixels to inspect; exclude polygons remove pixels from that mask. The detector builds a running background model, filters noise, measures connected regions, and applies trigger/clear frame counts before emitting `motion_start` or `motion_end`. JPEG snapshots are buffered separately (1280×720 at 10 fps by default). When RTSP is enabled, the H.264 channel runs at 1920×1080 and 30 fps. The RTSP code contains frame synchronization and SEI support.

## Configuration

The app reads `MOTION_CONFIG_PATH` (default `/userdata/motion_roi.json`) at startup. If the file is absent or invalid, built-in defaults apply. `main/default_motion_roi.json` is an example, not an automatically loaded fallback. Motion and notification settings can be changed through the HTTP API; RTSP changes are saved but need a service restart.

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
      "name": "Door",
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

Zone coordinates are normalized to 0–1. Each zone needs at least three points. An `include` zone is monitored; an `exclude` zone is ignored. The first `warmup_frames` frames after startup or background reset build the background model without alerting.

The `rtsp` block defaults to enabled on port 8554 at `/live0`. Its path must be one slash-prefixed segment containing only ASCII letters, digits, `_`, or `-` (at most 63 characters after the slash). A configuration update returns `rtsp.restart_required` when the active listener differs from the saved settings. Set `rtsp.enabled` to `false` and restart to stop the encoder and listener.

Other startup variables are `MOTION_HTTP_PORT` (default 8080), `MOTION_DEVICE_IP` (override the advertised address), `MOTION_SNAPSHOT_WIDTH` (1280), `MOTION_SNAPSHOT_HEIGHT` (720), and `MOTION_SNAPSHOT_FPS` (10; set to 0 to disable snapshots).

This build has no MQTT client. `/api/health` reports `mqtt.supported: false`, and a configuration containing an `mqtt` block is rejected.

## HTTP API

Default base URL: `http://<device-ip>:8080`. Responses are JSON except JPEG snapshots. The server sends CORS headers and handles `OPTIONS` requests.

| Method | Route | Purpose |
| --- | --- | --- |
| GET | `/api/health` | Reachability, active RTSP and snapshot state, notification state, MQTT capability |
| GET | `/api/version` | Program and API versions |
| GET | `/api/status` | Current detection state and latest measurements |
| GET / POST | `/api/config` | Read or save motion, zone, notification, and RTSP settings |
| POST | `/api/background/reset` | Rebuild the background model |
| GET | `/api/snapshot.jpg` | Latest JPEG, or `?event_id=N` / `?at=<unix_ms>` from the buffer |
| GET | `/api/events` | Event history; `?after_id=N&limit=M` for incremental reads |
| GET | `/api/frames` | Recent analysis-frame diagnostics |
| GET / POST | `/api/notify` | Read delivery statistics or update the webhook target |
| POST | `/api/notify/test` | Queue a test notification |

A `POST /api/config` replaces unspecified motion fields with their defaults. When the `notify` or `rtsp` block is omitted, the existing block is preserved. Send the complete motion settings you intend to keep. A notification-only change can use `POST /api/notify`.

The webhook posts JSON containing the event type, event ID, capture time, motion counts, normalized bounding box, snapshot URL, and (when enabled and available) a base64 JPEG. The notifier uses a bounded background queue so delivery does not block frame processing. Use `GET /api/notify` for send and failure counts.

## Home Assistant and desktop editor

Use the [Home Assistant integration](https://github.com/yyling0101-a11y/recamera-ha) to draw regions, show the RTSP view, receive webhook alerts, and browse snapshots. The device API above can also be used directly.

The optional Windows editor uses PySide6. From `windows_ui/`:

```powershell
python -m pip install -r requirements.txt
python app.py --rtsp rtsp://192.168.42.1:8554/live0 --api http://192.168.42.1:8080
```

You can also start it with `run_windows.bat` and change the addresses in the window. Connect RTSP, draw an Include polygon, optionally draw Exclude polygons over areas with recurring motion, then upload to the device. Left-click adds a vertex; right-click, Backspace, or Z removes the last vertex; Esc cancels the current polygon. The UI stores normalized coordinates.


## Home Assistant application bundle / HA 应用配置

`HA_config/recamera-app.json` declares the app metadata and supported connections.
`HA_config/recamera-panel.js` provides the sandboxed app page in the reCamera
Home Assistant integration. Keep both files in ZIP uploads; the device-side
program and Solution `.deb` installation are separate.
