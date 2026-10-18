"""Real-time feedback scope based on pyqtgraph (https://github.com/pyqtgraph/pyqtgraph)."""

from __future__ import annotations

import time

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QAction, QColor, QIcon, QPainter, QPixmap
from PySide6.QtWidgets import (
    QComboBox,
    QDoubleSpinBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMenu,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

CHANNELS = (
    ("pos", "位置", "rad", "#4fc3f7"),
    ("vel", "速度", "rad/s", "#69f0ae"),
    ("volt", "力矩指令", "pu", "#ffab40"),
)

_CAP = 65536
_CH = {key: (title, unit, color) for key, title, unit, color in CHANNELS}


class _Ring:
    def __init__(self) -> None:
        self.t = np.zeros(_CAP, dtype=np.float64)
        self.frame = np.zeros(_CAP, dtype=np.int64)
        self.y = {key: np.zeros(_CAP, dtype=np.float64) for key, *_ in CHANNELS}
        self.i = 0
        self.n = 0
        self.next_frame = 0

    def clear(self) -> None:
        self.i = 0
        self.n = 0
        self.next_frame = 0

    def push(self, t: float, pos: float, vel: float, volt: float) -> None:
        idx = self.i
        self.t[idx] = t
        self.frame[idx] = self.next_frame
        self.y["pos"][idx] = pos
        self.y["vel"][idx] = vel
        self.y["volt"][idx] = volt
        self.next_frame += 1
        self.i = (idx + 1) % _CAP
        self.n = min(self.n + 1, _CAP)

    def arrays(self) -> tuple[np.ndarray, np.ndarray, dict[str, np.ndarray]]:
        n = self.n
        if n == 0:
            empty = np.zeros(0, dtype=np.float64)
            return empty, empty.astype(np.int64), {key: empty for key, *_ in CHANNELS}
        if n < _CAP:
            sl = slice(0, n)
            return self.t[sl], self.frame[sl], {k: v[sl] for k, v in self.y.items()}
        i = self.i
        t = np.concatenate((self.t[i:], self.t[:i]))
        fr = np.concatenate((self.frame[i:], self.frame[:i]))
        ys = {k: np.concatenate((v[i:], v[:i])) for k, v in self.y.items()}
        return t, fr, ys


class ScopeViewBox(pg.ViewBox):
    """Time-series view: wheel zooms X, Ctrl+wheel zooms Y."""

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.on_double_click = None

    def wheelEvent(self, ev, axis=None):
        ctrl = bool(ev.modifiers() & Qt.KeyboardModifier.ControlModifier)
        if axis is None:
            self.setMouseEnabled(x=not ctrl, y=ctrl)
        try:
            super().wheelEvent(ev, axis)
        finally:
            self.setMouseEnabled(x=True, y=True)

    def mouseDoubleClickEvent(self, ev):
        if callable(self.on_double_click):
            self.on_double_click()
        ev.accept()


class ScopeView(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._ring = _Ring()
        self._paused = False
        self._follow = True
        self._auto_y = True
        self._normalize = False
        self._cyclic = False
        self._fb_hz = 0.0
        self._t0 = time.monotonic()
        self._freeze_t = 0.0
        self._cursor_x: float | None = None
        self._enabled: set[str] = {"pos"}
        self._updating_range = False

        pg.setConfigOptions(antialias=True, background="#101218", foreground="#c8cdd6")

        root = QVBoxLayout(self)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(8)
        root.addLayout(self._toolbar())

        self._hint = QLabel("")
        self._hint.setObjectName("scopeHint")
        self._hint.setWordWrap(True)
        root.addWidget(self._hint)

        self._vb = ScopeViewBox()
        self._vb.on_double_click = lambda: self.set_follow(True)
        self._pw = pg.PlotWidget(viewBox=self._vb)
        self._plot = self._pw.getPlotItem()
        self._plot.showGrid(x=True, y=True, alpha=0.25)
        self._plot.setLabel("bottom", "t (s)")
        self._plot.setLabel("left", "值")
        self._plot.getAxis("left").setWidth(56)
        self._plot.addLegend(offset=(8, 8))
        self._plot.setMenuEnabled(False)
        self._plot.disableAutoRange()
        self._plot.setMouseEnabled(x=True, y=True)
        self._pw.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self._pw.customContextMenuRequested.connect(self._on_menu)
        self._vb.sigRangeChangedManually.connect(self._on_user_range)

        dash = pg.mkPen("#8b93a3", width=1, style=Qt.PenStyle.DashLine)
        self._vline = pg.InfiniteLine(angle=90, movable=False, pen=dash)
        self._vline.setVisible(False)
        self._plot.addItem(self._vline, ignoreBounds=True)

        self._curves: dict[str, pg.PlotDataItem] = {}
        for key, title, _unit, color in CHANNELS:
            curve = self._plot.plot(pen=pg.mkPen(color, width=1.6), name=title)
            curve.setDownsampling(auto=True, method="peak")
            curve.setClipToView(True)
            curve.setVisible(key in self._enabled)
            self._curves[key] = curve
        self._refresh_legend()

        root.addWidget(self._pw, 1)

        self._cursor = QLabel("光标 —")
        self._cursor.setObjectName("scopeCursor")
        root.addWidget(self._cursor)

        self._mouse = pg.SignalProxy(self._plot.scene().sigMouseMoved, rateLimit=40, slot=self._on_mouse)

        self._timer = QTimer(self)
        self._timer.setInterval(33)
        self._timer.timeout.connect(self._redraw)
        self._timer.start()
        self._update_hint()
        self._update_ch_label()

    def _toolbar(self) -> QHBoxLayout:
        bar = QHBoxLayout()
        bar.setSpacing(10)

        title = QLabel("实时示波")
        title.setObjectName("scopeTitle")
        bar.addWidget(title)

        bar.addWidget(self._sep())
        bar.addWidget(QLabel("横轴"))
        self.cmb_x = QComboBox()
        self.cmb_x.addItem("时间 t", "time")
        self.cmb_x.addItem("按帧", "frame")
        self.cmb_x.setMinimumWidth(96)
        self.cmb_x.currentIndexChanged.connect(self._on_x_mode)
        bar.addWidget(self.cmb_x)

        self.lbl_win = QLabel("窗口 s")
        bar.addWidget(self.lbl_win)
        self.sp_time = QDoubleSpinBox()
        self.sp_time.setRange(0.2, 60.0)
        self.sp_time.setSingleStep(0.5)
        self.sp_time.setValue(5.0)
        self.sp_time.setDecimals(1)
        bar.addWidget(self.sp_time)

        self.sp_frames = QSpinBox()
        self.sp_frames.setRange(50, 20000)
        self.sp_frames.setSingleStep(50)
        self.sp_frames.setValue(800)
        self.sp_frames.setVisible(False)
        bar.addWidget(self.sp_frames)

        self.lbl_ch = QLabel("")
        self.lbl_ch.setObjectName("scopeHint")
        bar.addWidget(self.lbl_ch)

        bar.addStretch(1)
        self.lbl_n = QLabel("0 点")
        self.lbl_n.setObjectName("scopeHint")
        bar.addWidget(self.lbl_n)

        self.btn_follow = QPushButton("跟随")
        self.btn_follow.setCheckable(True)
        self.btn_follow.setChecked(True)
        self.btn_follow.toggled.connect(self.set_follow)
        self.btn_pause = QPushButton("暂停")
        self.btn_pause.setCheckable(True)
        self.btn_pause.toggled.connect(self._on_pause)
        self.btn_clear = QPushButton("清空")
        self.btn_clear.setObjectName("ghost")
        self.btn_clear.clicked.connect(self._on_clear)
        bar.addWidget(self.btn_follow)
        bar.addWidget(self.btn_pause)
        bar.addWidget(self.btn_clear)
        return bar

    @staticmethod
    def _sep() -> QFrame:
        line = QFrame()
        line.setFrameShape(QFrame.Shape.VLine)
        line.setObjectName("vsep")
        return line

    @staticmethod
    def _swatch(color: str) -> QIcon:
        pm = QPixmap(12, 12)
        pm.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pm)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setBrush(QColor(color))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(0, 0, 12, 12, 3, 3)
        painter.end()
        return QIcon(pm)

    def _now(self) -> float:
        if self._paused:
            return self._freeze_t
        return time.monotonic() - self._t0

    def push_feedback(self, pos: float, vel: float, volt: float) -> None:
        if self._paused:
            return
        self._ring.push(self._now(), pos, vel, volt)

    def set_status(self, cyclic: bool, fb_hz: float) -> None:
        changed = (cyclic != self._cyclic) or (abs(fb_hz - self._fb_hz) >= 1.0)
        self._cyclic = cyclic
        self._fb_hz = fb_hz
        if changed:
            self._update_hint()

    def set_follow(self, follow: bool) -> None:
        self._follow = bool(follow)
        if self.btn_follow.isChecked() != self._follow:
            self.btn_follow.blockSignals(True)
            self.btn_follow.setChecked(self._follow)
            self.btn_follow.blockSignals(False)
        if self._follow:
            self._redraw()

    def _on_user_range(self, *_args) -> None:
        if self._updating_range or not self._follow:
            return
        self.set_follow(False)

    def _on_clear(self) -> None:
        self._ring.clear()
        self._t0 = time.monotonic()
        self._freeze_t = 0.0
        self._cursor_x = None
        self._cursor.setText("光标 —")
        for curve in self._curves.values():
            curve.setData([], [])
        self.set_follow(True)

    def _on_pause(self, paused: bool) -> None:
        self._paused = paused
        self._freeze_t = time.monotonic() - self._t0
        self.btn_pause.setText("继续" if paused else "暂停")

    def _on_x_mode(self) -> None:
        time_mode = self.cmb_x.currentData() == "time"
        self.sp_time.setVisible(time_mode)
        self.sp_frames.setVisible(not time_mode)
        self.lbl_win.setText("窗口 s" if time_mode else "窗口 帧")
        self._plot.setLabel("bottom", "t (s)" if time_mode else "frame")
        self.set_follow(True)

    def _set_channel(self, key: str, enabled: bool) -> None:
        if enabled:
            self._enabled.add(key)
        else:
            self._enabled.discard(key)
        self._curves[key].setVisible(enabled)
        if not enabled:
            self._curves[key].setData([], [])
        self._refresh_legend()
        self._update_ch_label()
        self._update_left_label()
        if self._follow and self._auto_y:
            self._redraw()

    def _refresh_legend(self) -> None:
        legend = self._plot.legend
        if legend is None:
            return
        legend.clear()
        for key, title, _unit, _color in CHANNELS:
            if key in self._enabled:
                legend.addItem(self._curves[key], title)

    def _update_ch_label(self) -> None:
        if not self._enabled:
            self.lbl_ch.setText("右键图内添加通道")
            return
        names = [title for key, title, *_ in CHANNELS if key in self._enabled]
        self.lbl_ch.setText("通道：" + " / ".join(names))

    def _update_left_label(self) -> None:
        if self._normalize:
            self._plot.setLabel("left", "归一化")
            return
        units = [unit for key, _title, unit, _c in CHANNELS if key in self._enabled]
        units = list(dict.fromkeys(units))
        self._plot.setLabel("left", " / ".join(units) if units else "值")

    def _update_hint(self) -> None:
        extra = "滚轮缩放横轴，Ctrl+滚轮缩放纵轴；拖动画布平移；双击恢复跟随。右键选择要显示的数据。"
        if self._cyclic:
            self._hint.setText(f"循环发送已开 · 反馈约 {self._fb_hz:.0f} Hz。{extra}")
            self._hint.setProperty("warn", False)
        else:
            self._hint.setText(
                "要快速刷新曲线，请在 Motion / Velocity / Position 页打开「开始循环」。"
                f" 当前反馈约 {self._fb_hz:.0f} Hz。{extra}"
            )
            self._hint.setProperty("warn", True)
        self._hint.style().unpolish(self._hint)
        self._hint.style().polish(self._hint)

    def _on_menu(self, pos) -> None:
        menu = QMenu(self)
        vis = menu.addMenu("可视化数据")
        for key, title, unit, color in CHANNELS:
            act = QAction(self._swatch(color), f"{title}  ({unit})", vis)
            act.setCheckable(True)
            act.setChecked(key in self._enabled)
            act.toggled.connect(lambda checked, k=key: self._set_channel(k, checked))
            vis.addAction(act)

        menu.addSeparator()
        act_follow = menu.addAction("跟随最新")
        act_follow.setCheckable(True)
        act_follow.setChecked(self._follow)
        act_follow.toggled.connect(self.set_follow)

        act_y = menu.addAction("Y 自动（仅跟随）")
        act_y.setCheckable(True)
        act_y.setChecked(self._auto_y)
        act_y.toggled.connect(self._set_auto_y)

        act_n = menu.addAction("归一化叠加")
        act_n.setCheckable(True)
        act_n.setChecked(self._normalize)
        act_n.toggled.connect(self._set_normalize)

        menu.addSeparator()
        menu.addAction("复位视图", lambda: self.set_follow(True))
        menu.addAction("清空", self._on_clear)
        menu.exec(self._pw.mapToGlobal(pos))

    def _set_auto_y(self, enabled: bool) -> None:
        self._auto_y = enabled
        if self._follow:
            self._redraw()

    def _set_normalize(self, enabled: bool) -> None:
        self._normalize = enabled
        self._update_left_label()
        self._redraw()

    def _on_mouse(self, evt) -> None:
        pos = evt[0]
        if not self._plot.sceneBoundingRect().contains(pos):
            self._cursor_x = None
            self._vline.setVisible(False)
            self._cursor.setText("光标 —")
            return
        x = float(self._vb.mapSceneToView(pos).x())
        self._cursor_x = x
        self._vline.setVisible(True)
        self._vline.setPos(x)

    def _follow_span(self) -> tuple[float, float]:
        now = self._now()
        if self.cmb_x.currentData() == "time":
            win = float(self.sp_time.value())
            return now - win, now
        n = float(self.sp_frames.value())
        latest = float(self._ring.next_frame - 1) if self._ring.n else 0.0
        return latest - n + 1.0, latest

    def _slice(
        self, x: np.ndarray, ys: dict[str, np.ndarray], x_min: float, x_max: float
    ) -> tuple[np.ndarray, dict[str, np.ndarray]]:
        if x.size == 0:
            return x, {k: ys[k] for k in ys}
        pad = max((x_max - x_min) * 0.05, 1e-6)
        lo = int(np.searchsorted(x, x_min - pad, side="left"))
        hi = int(np.searchsorted(x, x_max + pad, side="right"))
        return x[lo:hi], {k: v[lo:hi] for k, v in ys.items()}

    def _scale_y(self, y: np.ndarray) -> np.ndarray:
        if not self._normalize or y.size == 0:
            return y
        lo = float(np.min(y))
        hi = float(np.max(y))
        span = hi - lo
        if span < 1e-12:
            return np.zeros_like(y)
        return (y - lo) / span

    def _apply_x_range(self, x_min: float, x_max: float) -> None:
        if x_max <= x_min:
            x_max = x_min + 1.0
        self._updating_range = True
        try:
            self._plot.setXRange(x_min, x_max, padding=0.0)
        finally:
            self._updating_range = False

    def _apply_y_range(self, values: list[np.ndarray]) -> None:
        if not self._auto_y:
            return
        chunks = [v for v in values if v.size]
        if not chunks:
            return
        lo = min(float(np.min(v)) for v in chunks)
        hi = max(float(np.max(v)) for v in chunks)
        if hi - lo < 1e-9:
            pad = 0.1 if abs(hi) < 1e-9 else abs(hi) * 0.1
            lo, hi = lo - pad, hi + pad
        else:
            pad = (hi - lo) * 0.08
            lo, hi = lo - pad, hi + pad
        self._updating_range = True
        try:
            self._plot.setYRange(lo, hi, padding=0.0)
        finally:
            self._updating_range = False

    def _redraw(self) -> None:
        t, frames, ys = self._ring.arrays()
        time_mode = self.cmb_x.currentData() == "time"
        x_all = t if time_mode else frames.astype(np.float64)
        self.lbl_n.setText(f"{int(self._ring.n)} 点")

        if self._follow:
            x_min, x_max = self._follow_span()
            x, ywin = self._slice(x_all, ys, x_min, x_max)
            self._apply_x_range(x_min, x_max)
        else:
            (x_min, x_max), _yr = self._vb.viewRange()
            x, ywin = self._slice(x_all, ys, float(x_min), float(x_max))
            if x.size == 0:
                x, ywin = x_all, ys

        shown: list[np.ndarray] = []
        for key, curve in self._curves.items():
            if key not in self._enabled:
                continue
            y = self._scale_y(ywin[key])
            curve.setData(x, y)
            shown.append(y)

        if self._follow:
            self._apply_y_range(shown)

        self._update_cursor_text(x, {k: self._scale_y(ywin[k]) for k in ywin})

    def _sample_at(self, x: np.ndarray, ys: dict[str, np.ndarray], xq: float) -> dict[str, float] | None:
        if x.size == 0:
            return None
        idx = int(np.searchsorted(x, xq))
        if idx <= 0:
            i = 0
        elif idx >= x.size:
            i = x.size - 1
        else:
            i = idx if abs(x[idx] - xq) < abs(x[idx - 1] - xq) else idx - 1
        return {key: float(ys[key][i]) for key in ys}

    def _update_cursor_text(self, x: np.ndarray, ys: dict[str, np.ndarray]) -> None:
        xq = self._cursor_x if self._cursor_x is not None else (float(x[-1]) if x.size else None)
        vals = self._sample_at(x, ys, xq) if xq is not None else None
        if vals is None:
            self._cursor.setText("光标 —")
            return
        axis = "t" if self.cmb_x.currentData() == "time" else "frame"
        parts = [f"{axis}={xq:.3f}" if axis == "t" else f"{axis}={xq:.0f}"]
        for key, title, unit, _color in CHANNELS:
            if key not in self._enabled:
                continue
            suffix = "" if self._normalize else f" {unit}"
            parts.append(f"{title} {vals[key]:.4f}{suffix}")
        self._cursor.setText("  ·  ".join(parts))
