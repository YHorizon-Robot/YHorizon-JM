"""RS05 / CyberGear / DM4310 关节调试上位机。

在 sdk/python 目录下运行：
    python joint_gui.py
    python joint_gui.py --interface virtual --channel vcan0 --protocol RS05 --sim
    python joint_gui.py --interface pcan --channel PCAN_USBBUS1 --node 1 --host 0xFD

只做常用控制与反馈显示：使能/失能/标零/清故障、位置/速度/阻抗/力矩指令、
位置/速度/力矩/温度/状态显示和三条滚动曲线。协议编解码在 joint_protocols.py。
"""

from __future__ import annotations

import argparse
import sys
import time
from collections import deque

import numpy as np
import pyqtgraph as pg
from PySide6 import QtCore, QtGui, QtWidgets

from can_backends import (
    BITRATES,
    INTERFACES,
    default_channel,
    default_interface,
    list_channels,
)
from joint_bus import BusLink, Simulator
from joint_protocols import (
    Command,
    Dm4310,
    Feedback,
    Rs05,
    describe_fault,
    protocol_by_name,
)


def _hex(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("YHorizon-JM · RS05 / CyberGear / DM4310 调试上位机")
        self.resize(1200, 780)

        self.protocol: Rs05 | Dm4310 = protocol_by_name("RS05")
        self.link: BusLink | None = None
        self.sim: Simulator | None = None
        self.connected = False
        self.last_feedback: Feedback | None = None

        self._t0 = time.monotonic()
        self._hist_t: deque[float] = deque(maxlen=4000)
        self._hist_p: deque[float] = deque(maxlen=4000)
        self._hist_v: deque[float] = deque(maxlen=4000)
        self._hist_tau: deque[float] = deque(maxlen=4000)
        self._fb_times: deque[float] = deque(maxlen=400)
        self._rx_count = 0
        self._last_tx_log = 0.0
        self._tx_skipped = 0
        self._last_fault_text = ""

        self._build_ui()
        self._refresh_channels()
        self._apply_protocol()
        self._set_connected(False)
        self._check_sim_available()

        self._redraw_timer = QtCore.QTimer(self)
        self._redraw_timer.timeout.connect(self._redraw)
        self._redraw_timer.start(40)

        self._stream_timer = QtCore.QTimer(self)
        self._stream_timer.timeout.connect(self._stream_tick)

    # ------------------------------------------------------------ UI

    def _build_ui(self) -> None:
        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        root = QtWidgets.QVBoxLayout(central)
        root.setContentsMargins(10, 8, 10, 8)
        root.setSpacing(8)

        root.addWidget(self._build_connection_group())

        splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        splitter.addWidget(self._build_control_group())
        splitter.addWidget(self._build_feedback_group())
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([360, 800])
        root.addWidget(splitter, 1)

        root.addWidget(self._build_log_group())

    def _build_connection_group(self) -> QtWidgets.QGroupBox:
        box = QtWidgets.QGroupBox("连接")
        grid = QtWidgets.QGridLayout(box)
        grid.setHorizontalSpacing(8)

        self.proto_combo = QtWidgets.QComboBox()
        self.proto_combo.addItems(["RS05", "CyberGear", "DM4310"])
        self.proto_combo.currentTextChanged.connect(self._on_protocol_changed)

        self.iface_combo = QtWidgets.QComboBox()
        self.iface_combo.addItems(INTERFACES)
        self.iface_combo.setCurrentText(default_interface())
        self.iface_combo.currentTextChanged.connect(self._refresh_channels)

        self.channel_combo = QtWidgets.QComboBox()
        self.channel_combo.setEditable(True)
        self.channel_combo.setMinimumWidth(130)

        self.bitrate_combo = QtWidgets.QComboBox()
        for rate in BITRATES:
            self.bitrate_combo.addItem(f"{rate // 1000} kbit/s", rate)
        self.bitrate_combo.setCurrentIndex(0)

        self.node_spin = QtWidgets.QSpinBox()
        self.node_spin.setRange(1, 63)
        self.node_spin.setValue(1)
        self.node_spin.setToolTip("电机节点 ID（固件 CFG_NODE_ID，1..63）")

        self.host_spin = QtWidgets.QSpinBox()
        self.host_spin.setValue(0xFD)
        self.node_spin.valueChanged.connect(self._on_node_changed)

        self.sim_check = QtWidgets.QCheckBox("内置模拟从站")
        self.sim_check.setToolTip("仅在 virtual 接口下可用，用于无硬件时验证界面")

        self.connect_btn = QtWidgets.QPushButton("连接")
        self.connect_btn.setMinimumWidth(88)
        self.connect_btn.clicked.connect(self._toggle_connect)

        self.conn_label = QtWidgets.QLabel("未连接")
        self.conn_label.setStyleSheet("color: #777;")

        row = 0
        grid.addWidget(QtWidgets.QLabel("协议"), row, 0)
        grid.addWidget(self.proto_combo, row, 1)
        grid.addWidget(QtWidgets.QLabel("接口"), row, 2)
        grid.addWidget(self.iface_combo, row, 3)
        grid.addWidget(QtWidgets.QLabel("通道"), row, 4)
        grid.addWidget(self.channel_combo, row, 5)
        grid.addWidget(QtWidgets.QLabel("波特率"), row, 6)
        grid.addWidget(self.bitrate_combo, row, 7)
        grid.addWidget(self.connect_btn, row, 8, 2, 1)

        row = 1
        grid.addWidget(QtWidgets.QLabel("节点 ID"), row, 0)
        grid.addWidget(self.node_spin, row, 1)
        self.host_label = QtWidgets.QLabel("主机 ID")
        grid.addWidget(self.host_label, row, 2)
        grid.addWidget(self.host_spin, row, 3)
        grid.addWidget(self.sim_check, row, 4, 1, 2)
        grid.addWidget(self.conn_label, row, 6, 1, 3)

        grid.setColumnStretch(9, 1)
        return box

    def _build_control_group(self) -> QtWidgets.QGroupBox:
        box = QtWidgets.QGroupBox("控制")
        layout = QtWidgets.QVBoxLayout(box)

        self.range_label = QtWidgets.QLabel("")
        self.range_label.setWordWrap(True)
        self.range_label.setStyleSheet("color: #666;")
        layout.addWidget(self.range_label)

        buttons = QtWidgets.QGridLayout()
        self.btn_enable = QtWidgets.QPushButton("使能")
        self.btn_disable = QtWidgets.QPushButton("失能")
        self.btn_zero = QtWidgets.QPushButton("标零")
        self.btn_clear = QtWidgets.QPushButton("清故障")
        self.btn_ping = QtWidgets.QPushButton("Ping")
        for i, btn in enumerate(
            (self.btn_enable, self.btn_disable, self.btn_zero, self.btn_clear, self.btn_ping)
        ):
            buttons.addWidget(btn, i // 3, i % 3)
        layout.addLayout(buttons)

        self.btn_enable.clicked.connect(self._send_enable)
        self.btn_disable.clicked.connect(self._send_disable)
        self.btn_zero.clicked.connect(self._send_zero)
        self.btn_clear.clicked.connect(self._send_clear)
        self.btn_ping.clicked.connect(self._send_ping)

        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        self.pos_spin = self._make_spin(3, -12.57, 12.57, 0.0)
        self.vel_spin = self._make_spin(2, -33.0, 33.0, 0.0)
        self.kp_spin = self._make_spin(3, 0.0, 500.0, 0.0)
        self.kd_spin = self._make_spin(3, 0.0, 5.0, 0.0)
        self.tau_spin = self._make_spin(3, -14.0, 14.0, 0.0)
        form.addRow("目标位置 (rad)", self.pos_spin)
        form.addRow("目标速度 (rad/s)", self.vel_spin)
        form.addRow("Kp (Nm/rad)", self.kp_spin)
        form.addRow("Kd (Nm·s/rad)", self.kd_spin)
        form.addRow("前馈力矩 (Nm)", self.tau_spin)
        layout.addLayout(form)

        presets = QtWidgets.QHBoxLayout()
        self.btn_damp = QtWidgets.QPushButton("阻尼")
        self.btn_hold = QtWidgets.QPushButton("保持")
        self.btn_free = QtWidgets.QPushButton("零力矩")
        self.btn_damp.clicked.connect(lambda: self._apply_preset(0.0, 1.0, 0.0))
        self.btn_hold.clicked.connect(lambda: self._apply_preset(10.0, 0.5, 0.0))
        self.btn_free.clicked.connect(lambda: self._apply_preset(0.0, 0.0, 0.0))
        self.btn_damp.setToolTip("Kp=0, Kd=1：只阻尼，可手推")
        self.btn_hold.setToolTip("Kp=10, Kd=0.5：保持当前位置")
        self.btn_free.setToolTip("Kp、Kd 和前馈均为零。JM1 启用齿槽表时仍会补偿，关桥请用失能。")
        for btn in (self.btn_damp, self.btn_hold, self.btn_free):
            presets.addWidget(btn)
        layout.addLayout(presets)

        send_row = QtWidgets.QHBoxLayout()
        self.btn_send = QtWidgets.QPushButton("发送一次")
        self.btn_send.setMinimumHeight(32)
        self.btn_send.clicked.connect(self._send_control)
        self.stream_check = QtWidgets.QCheckBox("连续发送")
        self.stream_check.toggled.connect(self._toggle_stream)
        self.rate_spin = QtWidgets.QSpinBox()
        self.rate_spin.setRange(5, 500)
        self.rate_spin.setValue(100)
        self.rate_spin.setSuffix(" Hz")
        send_row.addWidget(self.btn_send, 1)
        send_row.addWidget(self.stream_check)
        send_row.addWidget(self.rate_spin)
        layout.addLayout(send_row)

        layout.addStretch(1)
        return box

    def _build_feedback_group(self) -> QtWidgets.QWidget:
        panel = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(panel)
        layout.setContentsMargins(0, 0, 0, 0)

        box = QtWidgets.QGroupBox("反馈")
        vbox = QtWidgets.QVBoxLayout(box)

        values = QtWidgets.QGridLayout()
        self.val_pos = self._make_value_label()
        self.val_deg = self._make_value_label()
        self.val_vel = self._make_value_label()
        self.val_tau = self._make_value_label()
        self.val_temp = self._make_value_label()
        self.val_rate = self._make_value_label()
        self.val_state = QtWidgets.QLabel("—")
        self.val_state.setStyleSheet("font-size: 16px; font-weight: 600;")
        self.val_fault = QtWidgets.QLabel("—")
        self.val_fault.setStyleSheet("font-size: 14px; color: #a33;")

        def add(row: int, col: int, title: str, widget: QtWidgets.QWidget) -> None:
            cap = QtWidgets.QLabel(title)
            cap.setStyleSheet("color: #777; font-size: 12px;")
            cell = QtWidgets.QVBoxLayout()
            cell.setSpacing(0)
            cell.addWidget(cap)
            cell.addWidget(widget)
            values.addLayout(cell, row, col)

        add(0, 0, "位置", self.val_pos)
        add(0, 1, "位置 (°)", self.val_deg)
        add(0, 2, "速度 (rad/s)", self.val_vel)
        add(0, 3, "力矩 (Nm)", self.val_tau)
        add(0, 4, "温度 (℃)", self.val_temp)
        add(0, 5, "反馈频率", self.val_rate)
        add(1, 0, "状态", self.val_state)
        values.addWidget(self.val_fault, 1, 1, 1, 5)
        vbox.addLayout(values)

        plot_bar = QtWidgets.QHBoxLayout()
        self.pause_plot = QtWidgets.QCheckBox("暂停曲线")
        self.btn_clear_plot = QtWidgets.QPushButton("清空曲线")
        self.btn_clear_plot.clicked.connect(self._clear_plots)
        plot_bar.addWidget(self.pause_plot)
        plot_bar.addWidget(self.btn_clear_plot)
        plot_bar.addStretch(1)
        vbox.addLayout(plot_bar)

        self.plot_pos = self._make_plot("位置 rad", show_time=False)
        self.plot_vel = self._make_plot("速度 rad/s", show_time=False)
        self.plot_tau = self._make_plot("力矩 Nm", show_time=True)
        self.curve_pos = self.plot_pos.plot(pen=pg.mkPen("#1f77b4", width=2))
        self.curve_vel = self.plot_vel.plot(pen=pg.mkPen("#2ca02c", width=2))
        self.curve_tau = self.plot_tau.plot(pen=pg.mkPen("#d62728", width=2))
        vbox.addWidget(self.plot_pos, 1)
        vbox.addWidget(self.plot_vel, 1)
        vbox.addWidget(self.plot_tau, 1)

        layout.addWidget(box, 1)
        return panel

    def _build_log_group(self) -> QtWidgets.QGroupBox:
        box = QtWidgets.QGroupBox("报文日志")
        layout = QtWidgets.QVBoxLayout(box)
        self.log_view = QtWidgets.QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(500)
        self.log_view.setFixedHeight(120)
        font = QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont)
        font.setPointSize(9)
        self.log_view.setFont(font)
        layout.addWidget(self.log_view)

        bar = QtWidgets.QHBoxLayout()
        self.log_fb_check = QtWidgets.QCheckBox("记录反馈帧")
        self.log_fb_check.setToolTip("反馈默认只刷新界面和曲线，勾选后每帧都写日志")
        self.btn_clear_log = QtWidgets.QPushButton("清空日志")
        self.btn_clear_log.clicked.connect(self.log_view.clear)
        bar.addWidget(self.log_fb_check)
        bar.addStretch(1)
        bar.addWidget(self.btn_clear_log)
        layout.addLayout(bar)
        return box

    @staticmethod
    def _make_spin(decimals: int, lo: float, hi: float, value: float) -> QtWidgets.QDoubleSpinBox:
        spin = QtWidgets.QDoubleSpinBox()
        spin.setDecimals(decimals)
        spin.setRange(lo, hi)
        spin.setSingleStep(0.1 if hi <= 50 else 1.0)
        spin.setValue(value)
        spin.setKeyboardTracking(False)
        return spin

    @staticmethod
    def _make_value_label() -> QtWidgets.QLabel:
        label = QtWidgets.QLabel("—")
        label.setStyleSheet("font-size: 20px; font-weight: 600;")
        label.setMinimumWidth(90)
        return label

    def _make_plot(self, title: str, show_time: bool = False) -> pg.PlotWidget:
        plot = pg.PlotWidget()
        plot.setBackground("#fafafa")
        plot.showGrid(x=True, y=True, alpha=0.25)
        plot.setTitle(title, size="9pt", color="#555")
        if show_time:
            plot.setLabel("bottom", "时间", units="s")
        plot.setMouseEnabled(x=True, y=False)
        plot.setMenuEnabled(False)
        plot.getPlotItem().hideButtons()
        return plot

    # ------------------------------------------------------------ 协议切换

    def _on_node_changed(self, node: int) -> None:
        if isinstance(self.protocol, Dm4310):
            self.host_spin.setValue(0x10 + node)

    def _on_protocol_changed(self, name: str) -> None:
        if self.connected:
            self.proto_combo.setCurrentText(self.protocol.name)
            return
        self.protocol = protocol_by_name(name)
        self._apply_protocol()
        for field in (self.pos_spin, self.vel_spin, self.kp_spin, self.kd_spin, self.tau_spin):
            field.setValue(0)

    def _apply_protocol(self) -> None:
        proto = self.protocol
        rng = proto.control
        is_rs05 = isinstance(proto, Rs05)

        self.pos_spin.setRange(-rng.p_max, rng.p_max)
        self.vel_spin.setRange(-rng.v_max, rng.v_max)
        self.kp_spin.setRange(0.0, rng.kp_max)
        self.kd_spin.setRange(0.0, rng.kd_max)
        self.tau_spin.setRange(-rng.t_max, rng.t_max)

        self.host_label.setText("主机 ID" if is_rs05 else "反馈 ID (MST_ID)")
        self.host_spin.setRange(0, 0xFF if is_rs05 else 0x7FF)
        self.host_spin.setValue(0xFD if is_rs05 else 0x10 + self.node_spin.value())
        self.btn_ping.setEnabled(is_rs05)

        if proto.name == "CyberGear":
            self.range_label.setText("CyberGear 控制及反馈量程：位置 ±4π rad · 速度 ±30 rad/s · 力矩 ±12 Nm")
        elif is_rs05:
            self.range_label.setText(
                "RS05 控制量程：位置 ±12.57 rad · 速度 ±33 rad/s · Kp 0..500 · Kd 0..5 · 力矩 ±14 Nm\n"
                "反馈量程：位置 ±4π · 速度 ±50 rad/s · 力矩 ±5.5 Nm；29 位扩展帧。"
            )
        else:
            self.range_label.setText(
                "DM4310 控制量程：位置 ±12.5 rad · 速度 ±30 rad/s · Kp 0..500 · Kd 0..5 · 力矩 ±10 Nm\n"
                "反馈同量程；JM1 台架镜像节点为 1..15，MST_ID = 0x10 + 节点号。"
            )
        self._clear_feedback()

    def _check_sim_available(self) -> None:
        is_virtual = self.iface_combo.currentText() == "virtual"
        self.sim_check.setEnabled(is_virtual)
        if not is_virtual:
            self.sim_check.setChecked(False)

    def _refresh_channels(self) -> None:
        iface = self.iface_combo.currentText()
        current = self.channel_combo.currentText()
        channels = list_channels(iface)
        self.channel_combo.blockSignals(True)
        self.channel_combo.clear()
        self.channel_combo.addItems(channels)
        if current and current in channels:
            self.channel_combo.setCurrentText(current)
        elif channels:
            self.channel_combo.setCurrentText(channels[0])
        self.channel_combo.blockSignals(False)
        if not current and channels:
            self.channel_combo.setCurrentText(default_channel(iface))
        self._check_sim_available()

    # ------------------------------------------------------------ 连接

    def _toggle_connect(self) -> None:
        if self.connected:
            self._disconnect()
        else:
            self._connect()

    def _connect(self) -> None:
        iface = self.iface_combo.currentText()
        channel = self.channel_combo.currentText().strip()
        bitrate = int(self.bitrate_combo.currentData())
        if not channel:
            QtWidgets.QMessageBox.warning(self, "连接", "请先选择或输入 CAN 通道。")
            return

        node = self.node_spin.value()
        host = self.host_spin.value()

        if self.sim_check.isChecked() and iface == "virtual":
            self.sim = Simulator(iface, channel, bitrate, self.protocol, node, host)
            self.sim.start()
            self._log(f"已启动模拟从站 node={node} host=0x{host:02X}")

        self.link = BusLink(iface, channel, bitrate)
        self.link.opened.connect(self._on_link_opened)
        self.link.received.connect(self._on_frame)
        self.link.failed.connect(self._on_link_error)
        self.link.start()
        self.conn_label.setText("正在连接…")
        self.connect_btn.setEnabled(False)
        self._log(f"打开 {iface}:{channel} @ {bitrate // 1000} kbit/s")

    @QtCore.Slot(bool, str)
    def _on_link_opened(self, ok: bool, error: str) -> None:
        self.connect_btn.setEnabled(True)
        if not ok:
            self.conn_label.setText("连接失败")
            self.conn_label.setStyleSheet("color: #b00;")
            self._log(f"连接失败: {error}")
            self.statusBar().showMessage(f"连接失败：{error}", 8000)
            self._teardown_transport()
            return
        self._set_connected(True)
        self.conn_label.setText("已连接")
        self.conn_label.setStyleSheet("color: #0a0; font-weight: 600;")
        self._log("已连接")

    @QtCore.Slot(str)
    def _on_link_error(self, text: str) -> None:
        self._log(text)
        self.conn_label.setText("链路错误")
        self.conn_label.setStyleSheet("color: #b00;")

    def _disconnect(self) -> None:
        self.stream_check.setChecked(False)
        if self.link is not None:
            # 先失能再断开，避免电机保持最后一帧目标。
            try:
                arb, data = self._build_disable(clear_fault=False)
                self.link.send(arb, data, self.protocol.extended)
                if not self.sim:
                    self.link.msleep(20)
            except Exception:  # noqa: BLE001
                pass
        self._teardown_transport()
        self._set_connected(False)
        self.conn_label.setText("未连接")
        self.conn_label.setStyleSheet("color: #777;")
        self._log("已断开")

    def _teardown_transport(self) -> None:
        if self.link is not None:
            self.link.stop()
            self.link = None
        if self.sim is not None:
            self.sim.stop()
            self.sim = None

    def _set_connected(self, connected: bool) -> None:
        self.connected = connected
        self.proto_combo.setEnabled(not connected)
        self.iface_combo.setEnabled(not connected)
        self.channel_combo.setEnabled(not connected)
        self.bitrate_combo.setEnabled(not connected)
        self.node_spin.setEnabled(not connected)
        self.host_spin.setEnabled(not connected)
        self.sim_check.setEnabled(not connected and self.iface_combo.currentText() == "virtual")
        self.connect_btn.setText("断开" if connected else "连接")
        for widget in (
            self.btn_enable,
            self.btn_disable,
            self.btn_zero,
            self.btn_clear,
            self.btn_ping,
            self.btn_send,
            self.btn_damp,
            self.btn_hold,
            self.btn_free,
            self.stream_check,
            self.rate_spin,
        ):
            widget.setEnabled(connected)
        if connected:
            self.btn_ping.setEnabled(isinstance(self.protocol, Rs05))
        if not connected:
            self.stream_check.setChecked(False)

    # ------------------------------------------------------------ 指令

    def _command(self) -> Command:
        return Command(
            pos=self.pos_spin.value(),
            vel=self.vel_spin.value(),
            kp=self.kp_spin.value(),
            kd=self.kd_spin.value(),
            tau=self.tau_spin.value(),
        )

    def _send(self, arb: int, data: bytes, note: str, quiet: bool = False) -> None:
        if self.link is None:
            return
        self.link.send(arb, data, self.protocol.extended)
        now = time.monotonic()
        if quiet and (now - self._last_tx_log) < 0.5:
            self._tx_skipped += 1
            return
        self._last_tx_log = now
        extra = f"（另有 {self._tx_skipped} 帧未记录）" if self._tx_skipped else ""
        self._tx_skipped = 0
        kind = "EXT" if self.protocol.extended else "STD"
        self._log(f"TX {kind} 0x{arb:08X} [{_hex(data)}] {note}{extra}")

    def _build_disable(self, clear_fault: bool) -> tuple[int, bytes]:
        node = self.node_spin.value()
        host = self.host_spin.value()
        if isinstance(self.protocol, Rs05):
            return self.protocol.make_disable(node, host, clear_fault)
        if clear_fault:
            return self.protocol.make_clear_fault(node, host)
        return self.protocol.make_disable(node, host)

    def _send_enable(self) -> None:
        arb, data = self.protocol.make_enable(self.node_spin.value(), self.host_spin.value())
        self._send(arb, data, "使能")

    def _send_disable(self) -> None:
        self.stream_check.setChecked(False)
        arb, data = self._build_disable(clear_fault=False)
        self._send(arb, data, "失能")

    def _send_zero(self) -> None:
        arb, data = self.protocol.make_set_zero(self.node_spin.value(), self.host_spin.value())
        self._send(arb, data, "标零")

    def _send_clear(self) -> None:
        arb, data = self._build_disable(clear_fault=True)
        self._send(arb, data, "清故障")

    def _send_ping(self) -> None:
        if not isinstance(self.protocol, Rs05):
            return
        arb, data = self.protocol.make_ping(self.node_spin.value(), self.host_spin.value())
        self._send(arb, data, "Ping")

    def _send_control(self) -> None:
        cmd = self._command()
        arb, data = self.protocol.make_control(
            self.node_spin.value(), self.host_spin.value(), cmd
        )
        self._send(
            arb,
            data,
            f"pos={cmd.pos:.3f} vel={cmd.vel:.2f} kp={cmd.kp:.2f} kd={cmd.kd:.2f} tau={cmd.tau:.2f}",
            quiet=self.stream_check.isChecked(),
        )

    def _toggle_stream(self, on: bool) -> None:
        if on:
            interval = max(2, int(1000 / max(5, self.rate_spin.value())))
            self._stream_timer.start(interval)
        else:
            self._stream_timer.stop()

    def _stream_tick(self) -> None:
        if self.connected:
            self._send_control()

    def _apply_preset(self, kp: float, kd: float, tau: float) -> None:
        self.kp_spin.setValue(kp)
        self.kd_spin.setValue(kd)
        self.tau_spin.setValue(tau)

    # ------------------------------------------------------------ 反馈

    @QtCore.Slot(int, bytes, bool)
    def _on_frame(self, arb: int, data: bytes, extended: bool) -> None:
        self._rx_count += 1
        if (isinstance(self.protocol, Rs05) and extended and len(data) == 8
                and arb == ((self.node_spin.value() << 8) | 0xFE)):
            self._log(f"RX EXT 0x{arb:08X} [{_hex(data)}] Ping 应答")
            return

        fb = None
        if isinstance(self.protocol, Rs05):
            if (extended and len(data) == 8 and (arb & 0xFFFF) ==
                    ((self.node_spin.value() << 8) | self.host_spin.value())):
                fb = self.protocol.parse_feedback(arb, data)
            kind = "EXT" if extended else "STD"
        else:
            if (not extended and arb == self.host_spin.value() and len(data) == 8
                    and (data[0] & 0x0F) == (self.node_spin.value() & 0x0F)):
                fb = self.protocol.parse_feedback(arb, data)
            kind = "STD" if not extended else "EXT"

        if fb is None:
            self._log(f"RX {kind} 0x{arb:08X} [{_hex(data)}]")
            return

        fault_text = describe_fault(self.protocol, fb)
        if self.log_fb_check.isChecked() or (fault_text and fault_text != self._last_fault_text):
            self._log(f"RX {kind} 0x{arb:08X} [{_hex(data)}] fb")
        self._last_fault_text = fault_text
        self.last_feedback = fb
        self._fb_times.append(time.monotonic())
        self._update_values(fb)

        t = time.monotonic() - self._t0
        self._hist_t.append(t)
        self._hist_p.append(fb.pos)
        self._hist_v.append(fb.vel)
        self._hist_tau.append(fb.tau)

    def _update_values(self, fb: Feedback) -> None:
        self.val_pos.setText(f"{fb.pos:+.4f}")
        self.val_deg.setText(f"{fb.pos * 57.29578:+.2f}")
        self.val_vel.setText(f"{fb.vel:+.3f}")
        self.val_tau.setText(f"{fb.tau:+.3f}")
        self.val_temp.setText(f"{fb.temp_c:.1f}")

        fault_text = describe_fault(self.protocol, fb)
        if isinstance(self.protocol, Rs05):
            state = {0: "未使能", 1: "校准中", 2: "运行"}.get(fb.mode, f"模式 {fb.mode}")
        else:
            state = {0: "未使能", 1: "已使能"}.get(fb.status, f"状态 0x{fb.status:X}")
        self.val_state.setText(state)
        self.val_fault.setText(f"故障：{fault_text}" if fault_text else "")
        if fault_text:
            self.val_state.setStyleSheet("font-size: 16px; font-weight: 600; color: #b00;")
        else:
            self.val_state.setStyleSheet("font-size: 16px; font-weight: 600; color: #080;")

    def _clear_feedback(self) -> None:
        self.last_feedback = None
        self._last_fault_text = ""
        self._fb_times.clear()
        for label in (self.val_pos, self.val_deg, self.val_vel, self.val_tau, self.val_temp, self.val_rate):
            label.setText("—")
        self.val_state.setText("—")
        self.val_state.setStyleSheet("font-size: 16px; font-weight: 600;")
        self.val_fault.setText("")
        self._clear_plots()

    def _clear_plots(self) -> None:
        self._hist_t.clear()
        self._hist_p.clear()
        self._hist_v.clear()
        self._hist_tau.clear()
        for curve in (self.curve_pos, self.curve_vel, self.curve_tau):
            curve.setData([], [])

    def _redraw(self) -> None:
        now = time.monotonic()
        while self._fb_times and now - self._fb_times[0] > 1.0:
            self._fb_times.popleft()
        self.val_rate.setText(f"{len(self._fb_times)} Hz")

        if self.pause_plot.isChecked() or len(self._hist_t) < 2:
            return
        t = np.fromiter(self._hist_t, dtype=float)
        self.curve_pos.setData(t, np.fromiter(self._hist_p, dtype=float))
        self.curve_vel.setData(t, np.fromiter(self._hist_v, dtype=float))
        self.curve_tau.setData(t, np.fromiter(self._hist_tau, dtype=float))
        for plot in (self.plot_pos, self.plot_vel, self.plot_tau):
            plot.setXRange(max(0.0, t[-1] - 10.0), max(10.0, t[-1]), padding=0)

    # ------------------------------------------------------------ 其它

    def _log(self, text: str) -> None:
        stamp = time.strftime("%H:%M:%S", time.localtime())
        self.log_view.appendPlainText(f"{stamp} {text}")

    def set_protocol(self, name: str) -> None:
        self.proto_combo.setCurrentText(name)

    def set_connection(self, interface: str, channel: str, bitrate: int = 1_000_000) -> None:
        self.iface_combo.setCurrentText(interface)
        self.channel_combo.setCurrentText(channel)
        index = self.bitrate_combo.findData(bitrate)
        if index >= 0:
            self.bitrate_combo.setCurrentIndex(index)

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:  # noqa: N802 - Qt 命名
        if self.connected:
            self._disconnect()
        super().closeEvent(event)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="RS05 / CyberGear / DM4310 关节调试上位机")
    parser.add_argument("--interface", default=None, help="python-can 接口，如 pcan/slcan/socketcan/virtual")
    parser.add_argument("--channel", default=None, help="通道名，如 CAN1 / can0 / vcan0")
    parser.add_argument("--bitrate", type=int, default=1_000_000)
    parser.add_argument("--protocol", default="RS05", choices=["RS05", "CyberGear", "DM4310"])
    parser.add_argument("--node", type=int, default=1)
    parser.add_argument("--host", type=lambda x: int(x, 0), default=None)
    parser.add_argument("--sim", action="store_true", help="在 virtual 接口上启动内置模拟从站")
    parser.add_argument("--connect", action="store_true", help="启动后自动连接")
    args = parser.parse_args(argv)

    app = QtWidgets.QApplication(sys.argv[:1])
    app.setApplicationName("YHorizon Joint GUI")
    window = MainWindow()
    window.set_protocol(args.protocol)
    if args.interface:
        window.set_connection(args.interface, args.channel or "", args.bitrate)
    window.node_spin.setValue(args.node)
    if args.host is not None:
        window.host_spin.setValue(args.host)
    window.sim_check.setChecked(bool(args.sim))
    window.show()
    if args.connect:
        QtCore.QTimer.singleShot(100, window._connect)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
