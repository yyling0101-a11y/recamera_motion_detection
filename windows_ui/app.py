from __future__ import annotations

import argparse
import json
import sys
import uuid
from pathlib import Path
from typing import Any, Dict, List
from urllib.parse import urlparse

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

from api_client import DeviceApi, DeviceApiError
from status_poller import StatusPoller
from video_canvas import VideoCanvas
from video_worker import VideoWorker


def default_config() -> Dict[str, Any]:
    return {
        "version": 1,
        "enabled": True,
        "pixel_threshold": 20,
        "min_motion_pixels": 120,
        "min_blob_pixels": 40,
        "trigger_frames": 3,
        "clear_frames": 5,
        "warmup_frames": 20,
        "background_alpha": 0.04,
        "foreground_alpha": 0.002,
        "zones": [],
    }


class MainWindow(QMainWindow):
    def __init__(self, rtsp_url: str, api_url: str) -> None:
        super().__init__()
        self.setWindowTitle("reCamera 限定区域运动检测")
        self.resize(1500, 900)

        self.zones: List[Dict[str, Any]] = []
        self.video_worker: VideoWorker | None = None

        self.canvas = VideoCanvas()
        self.canvas.points_changed.connect(self._on_points_changed)

        self.rtsp_edit = QLineEdit(rtsp_url)
        self.api_edit = QLineEdit(api_url.rstrip("/"))
        self.api_edit.editingFinished.connect(self._api_url_changed)

        self.stream_button = QPushButton("连接 RTSP")
        self.stream_button.clicked.connect(self._toggle_stream)
        derive_button = QPushButton("从 RTSP 生成 API")
        derive_button.clicked.connect(self._derive_api_url)
        test_button = QPushButton("测试设备")
        test_button.clicked.connect(self._test_device)
        load_device_button = QPushButton("从设备读取")
        load_device_button.clicked.connect(self._load_from_device)
        upload_button = QPushButton("上传到设备")
        upload_button.clicked.connect(self._upload_to_device)
        reset_background_button = QPushButton("重建背景")
        reset_background_button.clicked.connect(self._reset_background)

        connection_group = QGroupBox("连接")
        connection_layout = QGridLayout(connection_group)
        connection_layout.addWidget(QLabel("RTSP URL"), 0, 0)
        connection_layout.addWidget(self.rtsp_edit, 0, 1, 1, 5)
        connection_layout.addWidget(self.stream_button, 0, 6)
        connection_layout.addWidget(QLabel("设备 API"), 1, 0)
        connection_layout.addWidget(self.api_edit, 1, 1, 1, 5)
        connection_layout.addWidget(derive_button, 1, 6)
        connection_layout.addWidget(test_button, 2, 1)
        connection_layout.addWidget(load_device_button, 2, 2)
        connection_layout.addWidget(upload_button, 2, 3)
        connection_layout.addWidget(reset_background_button, 2, 4)

        self.zone_type_combo = QComboBox()
        self.zone_type_combo.addItem("检测区域（Include）", "include")
        self.zone_type_combo.addItem("白名单/忽略区域（Exclude）", "exclude")
        self.zone_type_combo.currentIndexChanged.connect(self._draw_type_changed)
        self.zone_name_edit = QLineEdit()
        self.zone_name_edit.setPlaceholderText("例如：仓库入口、风扇白名单")
        self.point_count_label = QLabel("当前多边形：0 个点")

        finish_button = QPushButton("完成当前多边形")
        finish_button.clicked.connect(self._finish_polygon)
        undo_button = QPushButton("撤销一个点")
        undo_button.clicked.connect(self.canvas.undo_current_point)
        cancel_button = QPushButton("取消当前绘制")
        cancel_button.clicked.connect(self.canvas.clear_current)
        delete_button = QPushButton("删除选中区域")
        delete_button.clicked.connect(self._delete_selected_zone)

        zone_group = QGroupBox("区域编辑")
        zone_layout = QVBoxLayout(zone_group)
        zone_form = QFormLayout()
        zone_form.addRow("区域类型", self.zone_type_combo)
        zone_form.addRow("区域名称", self.zone_name_edit)
        zone_layout.addLayout(zone_form)
        zone_layout.addWidget(self.point_count_label)
        zone_layout.addWidget(QLabel("在画面中依次左键添加顶点；右键或 Backspace 撤销；至少 3 个点。"))
        row = QHBoxLayout()
        row.addWidget(finish_button)
        row.addWidget(undo_button)
        zone_layout.addLayout(row)
        row2 = QHBoxLayout()
        row2.addWidget(cancel_button)
        row2.addWidget(delete_button)
        zone_layout.addLayout(row2)

        self.zone_list = QListWidget()
        self.zone_list.currentRowChanged.connect(self._zone_selection_changed)
        zone_layout.addWidget(self.zone_list, 1)

        self.enabled_check = QCheckBox("启用运动检测")
        self.pixel_threshold = self._spin(1, 255, 20)
        self.min_motion_pixels = self._spin(1, 1_000_000, 120)
        self.min_blob_pixels = self._spin(1, 1_000_000, 40)
        self.trigger_frames = self._spin(1, 1000, 3)
        self.clear_frames = self._spin(1, 1000, 5)
        self.warmup_frames = self._spin(0, 10000, 20)
        self.background_alpha = self._double_spin(0.001, 1.0, 0.04, 3)
        self.foreground_alpha = self._double_spin(0.0, 1.0, 0.002, 4)

        parameter_group = QGroupBox("检测参数")
        parameter_layout = QFormLayout(parameter_group)
        parameter_layout.addRow(self.enabled_check)
        parameter_layout.addRow("像素差阈值", self.pixel_threshold)
        parameter_layout.addRow("最少运动像素", self.min_motion_pixels)
        parameter_layout.addRow("最小连通块面积", self.min_blob_pixels)
        parameter_layout.addRow("连续触发帧", self.trigger_frames)
        parameter_layout.addRow("连续清除帧", self.clear_frames)
        parameter_layout.addRow("背景预热帧", self.warmup_frames)
        parameter_layout.addRow("静态背景更新率", self.background_alpha)
        parameter_layout.addRow("前景慢更新率", self.foreground_alpha)

        save_button = QPushButton("保存本地 JSON")
        save_button.clicked.connect(self._save_local)
        open_button = QPushButton("打开本地 JSON")
        open_button.clicked.connect(self._open_local)
        local_row = QHBoxLayout()
        local_row.addWidget(open_button)
        local_row.addWidget(save_button)

        self.api_state_label = QLabel("设备 API：等待连接")
        self.stream_state_label = QLabel("RTSP：未连接")
        self.motion_state_label = QLabel("状态：未知")
        self.metrics_label = QLabel("运动像素：-  最大连通块：-  掩码像素：-")
        self.metrics_label.setWordWrap(True)

        status_group = QGroupBox("实时状态")
        status_layout = QVBoxLayout(status_group)
        status_layout.addWidget(self.api_state_label)
        status_layout.addWidget(self.stream_state_label)
        status_layout.addWidget(self.motion_state_label)
        status_layout.addWidget(self.metrics_label)

        left_panel = QWidget()
        left_layout = QVBoxLayout(left_panel)
        left_layout.addWidget(zone_group, 3)
        left_layout.addWidget(parameter_group, 2)
        left_layout.addLayout(local_row)
        left_layout.addWidget(status_group)
        left_panel.setMinimumWidth(390)
        left_panel.setMaximumWidth(500)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(left_panel)
        splitter.addWidget(self.canvas)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)

        central = QWidget()
        main_layout = QVBoxLayout(central)
        main_layout.addWidget(connection_group)
        main_layout.addWidget(splitter, 1)
        self.setCentralWidget(central)

        self._apply_config(default_config())
        self._draw_type_changed()

        self.status_poller = StatusPoller(self.api_edit.text().strip())
        self.status_poller.status_ready.connect(self._status_updated)
        self.status_poller.connection_state.connect(self._api_state_changed)
        self.status_poller.start()

    @staticmethod
    def _spin(minimum: int, maximum: int, value: int) -> QSpinBox:
        box = QSpinBox()
        box.setRange(minimum, maximum)
        box.setValue(value)
        return box

    @staticmethod
    def _double_spin(minimum: float, maximum: float, value: float, decimals: int) -> QDoubleSpinBox:
        box = QDoubleSpinBox()
        box.setRange(minimum, maximum)
        box.setDecimals(decimals)
        box.setSingleStep(10 ** (-decimals))
        box.setValue(value)
        return box

    def _device_api(self) -> DeviceApi:
        base_url = self.api_edit.text().strip().rstrip("/")
        if not base_url.startswith(("http://", "https://")):
            raise DeviceApiError("设备 API 必须以 http:// 或 https:// 开头")
        return DeviceApi(base_url)

    def _api_url_changed(self) -> None:
        self.status_poller.set_base_url(self.api_edit.text().strip())

    def _derive_api_url(self) -> None:
        parsed = urlparse(self.rtsp_edit.text().strip())
        if not parsed.hostname:
            QMessageBox.warning(self, "URL 错误", "无法从 RTSP URL 中解析设备 IP。")
            return
        self.api_edit.setText(f"http://{parsed.hostname}:8080")
        self._api_url_changed()

    def _toggle_stream(self) -> None:
        if self.video_worker is not None:
            self.video_worker.stop()
            self.video_worker.wait(3000)
            self.video_worker = None
            self.stream_button.setText("连接 RTSP")
            return

        url = self.rtsp_edit.text().strip()
        if not url:
            QMessageBox.warning(self, "RTSP URL", "请输入 RTSP URL。")
            return
        self.video_worker = VideoWorker(url, self)
        self.video_worker.frame_ready.connect(self.canvas.set_frame)
        self.video_worker.state_changed.connect(self._stream_state_changed)
        self.video_worker.finished.connect(self._video_worker_finished)
        self.video_worker.start()
        self.stream_button.setText("停止 RTSP")

    def _video_worker_finished(self) -> None:
        if self.video_worker is not None and not self.video_worker.isRunning():
            self.video_worker = None
            self.stream_button.setText("连接 RTSP")

    def _stream_state_changed(self, message: str) -> None:
        self.stream_state_label.setText(f"RTSP：{message}")

    def _api_state_changed(self, message: str) -> None:
        self.api_state_label.setText(f"设备 API：{message}")

    def _test_device(self) -> None:
        try:
            health = self._device_api().health()
        except DeviceApiError as exc:
            QMessageBox.critical(self, "连接失败", str(exc))
            return
        QMessageBox.information(
            self,
            "设备连接正常",
            f"设备 IP：{health.get('device_ip', '-')}\n"
            f"RTSP：{health.get('rtsp_url', '-')}\n"
            f"分析分辨率：{health.get('analysis_width', '-')}×{health.get('analysis_height', '-')}"
        )

    def _draw_type_changed(self) -> None:
        self.canvas.set_draw_type(str(self.zone_type_combo.currentData()))

    def _on_points_changed(self, count: int) -> None:
        self.point_count_label.setText(f"当前多边形：{count} 个点")

    def _finish_polygon(self) -> None:
        points = self.canvas.current_points()
        if len(points) < 3:
            QMessageBox.warning(self, "顶点不足", "一个多边形至少需要 3 个顶点。")
            return

        zone_type = str(self.zone_type_combo.currentData())
        default_name = "检测区域" if zone_type == "include" else "白名单区域"
        name = self.zone_name_edit.text().strip() or f"{default_name} {len(self.zones) + 1}"
        zone = {
            "id": f"zone-{uuid.uuid4().hex[:10]}",
            "name": name,
            "type": zone_type,
            "points": [[round(x, 6), round(y, 6)] for x, y in points],
        }
        self.zones.append(zone)
        self.canvas.clear_current()
        self.zone_name_edit.clear()
        self._refresh_zone_list(select_index=len(self.zones) - 1)

    def _refresh_zone_list(self, select_index: int = -1) -> None:
        self.zone_list.blockSignals(True)
        self.zone_list.clear()
        for zone in self.zones:
            prefix = "检测" if zone.get("type") == "include" else "白名单"
            item = QListWidgetItem(f"[{prefix}] {zone.get('name', zone.get('id', '未命名'))}")
            item.setToolTip(str(zone.get("id", "")))
            self.zone_list.addItem(item)
        self.zone_list.blockSignals(False)
        self.canvas.set_zones(self.zones)
        if 0 <= select_index < len(self.zones):
            self.zone_list.setCurrentRow(select_index)
        else:
            self.canvas.set_selected_zone(-1)

    def _zone_selection_changed(self, row: int) -> None:
        self.canvas.set_selected_zone(row)

    def _delete_selected_zone(self) -> None:
        row = self.zone_list.currentRow()
        if row < 0 or row >= len(self.zones):
            QMessageBox.information(self, "删除区域", "请先在区域列表中选择一个区域。")
            return
        del self.zones[row]
        self._refresh_zone_list(select_index=min(row, len(self.zones) - 1))

    def _collect_config(self) -> Dict[str, Any]:
        return {
            "version": 1,
            "enabled": self.enabled_check.isChecked(),
            "pixel_threshold": self.pixel_threshold.value(),
            "min_motion_pixels": self.min_motion_pixels.value(),
            "min_blob_pixels": self.min_blob_pixels.value(),
            "trigger_frames": self.trigger_frames.value(),
            "clear_frames": self.clear_frames.value(),
            "warmup_frames": self.warmup_frames.value(),
            "background_alpha": self.background_alpha.value(),
            "foreground_alpha": self.foreground_alpha.value(),
            "zones": self.zones,
        }

    def _apply_config(self, config: Dict[str, Any]) -> None:
        self.enabled_check.setChecked(bool(config.get("enabled", True)))
        self.pixel_threshold.setValue(int(config.get("pixel_threshold", 20)))
        self.min_motion_pixels.setValue(int(config.get("min_motion_pixels", 120)))
        self.min_blob_pixels.setValue(int(config.get("min_blob_pixels", 40)))
        self.trigger_frames.setValue(int(config.get("trigger_frames", 3)))
        self.clear_frames.setValue(int(config.get("clear_frames", 5)))
        self.warmup_frames.setValue(int(config.get("warmup_frames", 20)))
        self.background_alpha.setValue(float(config.get("background_alpha", 0.04)))
        self.foreground_alpha.setValue(float(config.get("foreground_alpha", 0.002)))
        self.zones = list(config.get("zones", []))
        self.canvas.clear_current()
        self._refresh_zone_list()

    def _load_from_device(self) -> None:
        try:
            config = self._device_api().get_config()
            self._apply_config(config)
        except (DeviceApiError, ValueError, TypeError) as exc:
            QMessageBox.critical(self, "读取失败", str(exc))
            return
        QMessageBox.information(self, "读取完成", "已从 reCamera 读取区域和检测参数。")

    def _upload_to_device(self) -> None:
        include_count = sum(1 for zone in self.zones if zone.get("type") == "include")
        if self.enabled_check.isChecked() and include_count == 0:
            QMessageBox.warning(
                self,
                "没有检测区域",
                "当前没有 Include 检测区域。配置仍可上传，但设备不会触发运动报警。",
            )
        try:
            returned = self._device_api().set_config(self._collect_config())
            self._apply_config(returned)
        except DeviceApiError as exc:
            QMessageBox.critical(self, "上传失败", str(exc))
            return
        QMessageBox.information(self, "上传完成", "配置已保存到 reCamera，背景模型已自动重建。")

    def _reset_background(self) -> None:
        try:
            self._device_api().reset_background()
        except DeviceApiError as exc:
            QMessageBox.critical(self, "重建失败", str(exc))
            return
        QMessageBox.information(self, "背景重建", "设备已清空背景模型，将重新预热。")

    def _save_local(self) -> None:
        filename, _ = QFileDialog.getSaveFileName(
            self,
            "保存运动检测配置",
            "motion_roi.json",
            "JSON 文件 (*.json)",
        )
        if not filename:
            return
        try:
            Path(filename).write_text(
                json.dumps(self._collect_config(), ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
        except OSError as exc:
            QMessageBox.critical(self, "保存失败", str(exc))

    def _open_local(self) -> None:
        filename, _ = QFileDialog.getOpenFileName(
            self,
            "打开运动检测配置",
            "",
            "JSON 文件 (*.json)",
        )
        if not filename:
            return
        try:
            config = json.loads(Path(filename).read_text(encoding="utf-8"))
            if not isinstance(config, dict):
                raise ValueError("配置根节点必须是 JSON 对象")
            self._apply_config(config)
        except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
            QMessageBox.critical(self, "打开失败", str(exc))

    def _status_updated(self, status: Dict[str, Any]) -> None:
        motion = bool(status.get("motion", False))
        warming = bool(status.get("warming_up", False))
        configured = bool(status.get("configured", False))
        enabled = bool(status.get("enabled", False))

        if motion:
            text = f"状态：检测到运动（事件 #{status.get('event_id', 0)}）"
            self.motion_state_label.setStyleSheet("font-weight: bold; color: #d00000;")
        elif not enabled:
            text = "状态：检测已关闭"
            self.motion_state_label.setStyleSheet("")
        elif not configured:
            text = "状态：没有有效的 Include 检测区域"
            self.motion_state_label.setStyleSheet("")
        elif warming:
            text = "状态：背景模型预热中"
            self.motion_state_label.setStyleSheet("color: #9a6700;")
        else:
            text = "状态：正常，无运动"
            self.motion_state_label.setStyleSheet("color: #087830;")
        self.motion_state_label.setText(text)

        self.metrics_label.setText(
            "运动像素：{motion_pixels}  最大连通块：{blob}  掩码像素：{mask}  "
            "运动比例：{ratio:.4f}".format(
                motion_pixels=int(status.get("motion_pixels", 0)),
                blob=int(status.get("largest_blob_pixels", 0)),
                mask=int(status.get("active_mask_pixels", 0)),
                ratio=float(status.get("motion_ratio", 0.0)),
            )
        )
        self.canvas.set_motion_status(motion, status.get("bbox"))

    def closeEvent(self, event) -> None:  # noqa: N802
        if self.video_worker is not None:
            self.video_worker.stop()
            self.video_worker.wait(3000)
            self.video_worker = None
        self.status_poller.stop()
        self.status_poller.wait(2000)
        event.accept()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="reCamera ROI motion detection UI")
    parser.add_argument("--rtsp", default="rtsp://192.168.42.1:8554/live0")
    parser.add_argument("--api", default="http://192.168.42.1:8080")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    app = QApplication(sys.argv)
    app.setApplicationName("reCamera Motion UI")
    window = MainWindow(arguments.rtsp, arguments.api)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
