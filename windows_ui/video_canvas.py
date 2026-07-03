from __future__ import annotations

from typing import Dict, List, Optional, Sequence, Tuple

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QBrush, QColor, QImage, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QWidget

NormalizedPoint = Tuple[float, float]


class VideoCanvas(QWidget):
    points_changed = Signal(int)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setMinimumSize(800, 450)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

        self._image: Optional[QImage] = None
        self._zones: List[Dict] = []
        self._current_points: List[NormalizedPoint] = []
        self._current_type = "include"
        self._selected_zone = -1
        self._motion_bbox: Optional[Sequence[float]] = None
        self._motion_active = False

    def set_frame(self, image: QImage) -> None:
        self._image = image
        self.update()

    def set_zones(self, zones: List[Dict]) -> None:
        self._zones = zones
        self.update()

    def set_selected_zone(self, index: int) -> None:
        self._selected_zone = index
        self.update()

    def set_draw_type(self, zone_type: str) -> None:
        self._current_type = zone_type
        self.update()

    def set_motion_status(self, active: bool, bbox: Optional[Sequence[float]]) -> None:
        self._motion_active = active
        self._motion_bbox = bbox
        self.update()

    def current_points(self) -> List[NormalizedPoint]:
        return list(self._current_points)

    def clear_current(self) -> None:
        self._current_points.clear()
        self.points_changed.emit(0)
        self.update()

    def undo_current_point(self) -> None:
        if self._current_points:
            self._current_points.pop()
            self.points_changed.emit(len(self._current_points))
            self.update()

    def _display_rect(self) -> QRectF:
        area = QRectF(self.rect())
        if self._image is None or self._image.width() <= 0 or self._image.height() <= 0:
            return area
        image_ratio = self._image.width() / self._image.height()
        widget_ratio = area.width() / max(1.0, area.height())
        if widget_ratio > image_ratio:
            height = area.height()
            width = height * image_ratio
            return QRectF((area.width() - width) / 2.0, 0.0, width, height)
        width = area.width()
        height = width / image_ratio
        return QRectF(0.0, (area.height() - height) / 2.0, width, height)

    def _normalized_to_widget(self, point: NormalizedPoint) -> QPointF:
        rect = self._display_rect()
        return QPointF(rect.left() + point[0] * rect.width(), rect.top() + point[1] * rect.height())

    def _widget_to_normalized(self, point: QPointF) -> Optional[NormalizedPoint]:
        rect = self._display_rect()
        if not rect.contains(point) or rect.width() <= 0 or rect.height() <= 0:
            return None
        x = (point.x() - rect.left()) / rect.width()
        y = (point.y() - rect.top()) / rect.height()
        return max(0.0, min(1.0, x)), max(0.0, min(1.0, y))

    @staticmethod
    def _zone_colors(zone_type: str, selected: bool = False) -> Tuple[QColor, QColor]:
        if zone_type == "exclude":
            line = QColor(40, 170, 255) if not selected else QColor(120, 215, 255)
            fill = QColor(40, 170, 255, 45)
        else:
            line = QColor(255, 85, 50) if not selected else QColor(255, 210, 40)
            fill = QColor(255, 85, 50, 45)
        return line, fill

    def _draw_polygon(
        self,
        painter: QPainter,
        points: Sequence[NormalizedPoint],
        zone_type: str,
        selected: bool,
        closed: bool,
    ) -> None:
        if not points:
            return

        # 每个多边形单独保存/恢复 QPainter 状态。
        # 否则上一个多边形绘制顶点时设置的“不透明 brush”会被下一个
        # painter.drawPath() 继承，从而把第二个多边形重新填充成不透明颜色。
        painter.save()
        try:
            line, fill = self._zone_colors(zone_type, selected)
            mapped = [self._normalized_to_widget(point) for point in points]

            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            painter.setPen(QPen(line, 3 if selected else 2))

            if closed and len(mapped) >= 3:
                path = QPainterPath(mapped[0])
                for point in mapped[1:]:
                    path.lineTo(point)
                path.closeSubpath()

                # 先以带 alpha 的颜色填充。
                painter.fillPath(path, QBrush(fill))

                # drawPath 同时会使用当前 brush 填充路径，所以画轮廓前必须
                # 明确设置 NoBrush，保证这里只画边线，不再做一次不透明填充。
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.drawPath(path)
            else:
                painter.setBrush(Qt.BrushStyle.NoBrush)
                for first, second in zip(mapped, mapped[1:]):
                    painter.drawLine(first, second)

            # 顶点本身保持实心，便于用户观察和编辑。
            painter.setBrush(QBrush(line))
            painter.setPen(Qt.PenStyle.NoPen)
            radius = 4.0
            for point in mapped:
                painter.drawEllipse(point, radius, radius)
        finally:
            painter.restore()

    def paintEvent(self, event) -> None:  # noqa: N802
        del event
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(20, 20, 20))
        rect = self._display_rect()

        if self._image is not None:
            painter.drawImage(rect, self._image)
        else:
            painter.setPen(QColor(220, 220, 220))
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "点击“连接 RTSP”开始预览")

        for index, zone in enumerate(self._zones):
            points = [(float(p[0]), float(p[1])) for p in zone.get("points", [])]
            self._draw_polygon(
                painter,
                points,
                zone.get("type", "include"),
                selected=index == self._selected_zone,
                closed=True,
            )

        self._draw_polygon(
            painter,
            self._current_points,
            self._current_type,
            selected=True,
            closed=False,
        )

        if self._motion_bbox and len(self._motion_bbox) == 4:
            x, y, width, height = [float(value) for value in self._motion_bbox]
            top_left = self._normalized_to_widget((x, y))
            bottom_right = self._normalized_to_widget((x + width, y + height))
            bbox_rect = QRectF(top_left, bottom_right).normalized()
            color = QColor(255, 30, 30) if self._motion_active else QColor(255, 180, 30)
            painter.setPen(QPen(color, 4))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRect(bbox_rect)

        painter.setPen(QPen(QColor(90, 90, 90), 1))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRect(rect)

    def mousePressEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.RightButton:
            self.undo_current_point()
            return
        if event.button() != Qt.MouseButton.LeftButton:
            return
        normalized = self._widget_to_normalized(event.position())
        if normalized is None:
            return
        self._current_points.append(normalized)
        self.points_changed.emit(len(self._current_points))
        self.update()

    def keyPressEvent(self, event) -> None:  # noqa: N802
        if event.key() in (Qt.Key.Key_Backspace, Qt.Key.Key_Z):
            self.undo_current_point()
            return
        if event.key() == Qt.Key.Key_Escape:
            self.clear_current()
            return
        super().keyPressEvent(event)
