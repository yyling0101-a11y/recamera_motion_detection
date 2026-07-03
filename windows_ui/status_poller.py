from __future__ import annotations

import threading
import time
from typing import Optional

from PySide6.QtCore import QThread, Signal

from api_client import DeviceApi


class StatusPoller(QThread):
    status_ready = Signal(dict)
    connection_state = Signal(str)

    def __init__(self, base_url: str, interval: float = 0.5, parent=None) -> None:
        super().__init__(parent)
        self._base_url = base_url
        self._interval = interval
        self._lock = threading.Lock()
        self._running = True

    def set_base_url(self, base_url: str) -> None:
        with self._lock:
            self._base_url = base_url.rstrip("/")

    def stop(self) -> None:
        self._running = False
        self.requestInterruption()

    def run(self) -> None:
        last_error: Optional[str] = None
        while self._running and not self.isInterruptionRequested():
            with self._lock:
                base_url = self._base_url
            try:
                status = DeviceApi(base_url, timeout=0.8).get_status()
                self.status_ready.emit(status)
                if last_error is not None:
                    self.connection_state.emit("设备 API 已连接")
                last_error = None
            except Exception as exc:  # 状态轮询不能让线程退出
                message = str(exc)
                if message != last_error:
                    self.connection_state.emit(message)
                    last_error = message
            time.sleep(self._interval)
