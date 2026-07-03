from __future__ import annotations

import os
import time

import cv2
import numpy as np
from PySide6.QtCore import QThread, Signal
from PySide6.QtGui import QImage


class VideoWorker(QThread):
    frame_ready = Signal(QImage)
    state_changed = Signal(str)

    def __init__(self, rtsp_url: str, parent=None) -> None:
        super().__init__(parent)
        self.rtsp_url = rtsp_url
        self._running = True

    def stop(self) -> None:
        self._running = False
        self.requestInterruption()

    def _open_capture(self) -> cv2.VideoCapture:
        # 优先 TCP，减小局域网丢包导致的花屏；stimeout 单位为微秒。
        os.environ.setdefault(
            "OPENCV_FFMPEG_CAPTURE_OPTIONS",
            "rtsp_transport;tcp|stimeout;3000000|max_delay;500000",
        )
        capture = cv2.VideoCapture(self.rtsp_url, cv2.CAP_FFMPEG)
        capture.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        return capture

    def run(self) -> None:
        retry_delay = 0.5
        while self._running and not self.isInterruptionRequested():
            self.state_changed.emit("正在连接 RTSP…")
            capture = self._open_capture()
            if not capture.isOpened():
                self.state_changed.emit("RTSP 连接失败，正在重试…")
                capture.release()
                time.sleep(retry_delay)
                retry_delay = min(3.0, retry_delay * 1.5)
                continue

            retry_delay = 0.5
            self.state_changed.emit("RTSP 已连接")
            consecutive_failures = 0

            while self._running and not self.isInterruptionRequested():
                ok, frame = capture.read()
                if not ok or frame is None:
                    consecutive_failures += 1
                    if consecutive_failures >= 10:
                        self.state_changed.emit("RTSP 已断开，正在重连…")
                        break
                    time.sleep(0.03)
                    continue

                consecutive_failures = 0
                if not isinstance(frame, np.ndarray) or frame.ndim != 3:
                    continue
                rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                height, width, channels = rgb.shape
                image = QImage(
                    rgb.data,
                    width,
                    height,
                    channels * width,
                    QImage.Format.Format_RGB888,
                ).copy()
                self.frame_ready.emit(image)

            capture.release()

        self.state_changed.emit("RTSP 已停止")
