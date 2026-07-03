from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict

import requests


class DeviceApiError(RuntimeError):
    pass


@dataclass
class DeviceApi:
    base_url: str
    timeout: float = 2.0

    def __post_init__(self) -> None:
        self.base_url = self.base_url.rstrip("/")

    def _request(self, method: str, path: str, **kwargs: Any) -> Dict[str, Any]:
        try:
            response = requests.request(
                method,
                f"{self.base_url}{path}",
                timeout=self.timeout,
                **kwargs,
            )
        except requests.RequestException as exc:
            raise DeviceApiError(f"无法连接设备：{exc}") from exc

        try:
            payload = response.json()
        except ValueError as exc:
            raise DeviceApiError(
                f"设备返回的不是 JSON（HTTP {response.status_code}）：{response.text[:200]}"
            ) from exc

        if not response.ok:
            message = payload.get("error", payload)
            raise DeviceApiError(f"设备返回 HTTP {response.status_code}：{message}")
        return payload

    def health(self) -> Dict[str, Any]:
        return self._request("GET", "/api/health")

    def get_status(self) -> Dict[str, Any]:
        return self._request("GET", "/api/status")

    def get_config(self) -> Dict[str, Any]:
        return self._request("GET", "/api/config")

    def set_config(self, config: Dict[str, Any]) -> Dict[str, Any]:
        return self._request("POST", "/api/config", json=config)

    def reset_background(self) -> Dict[str, Any]:
        return self._request("POST", "/api/background/reset", json={})
