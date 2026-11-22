"""CAN 链路线程与内置模拟从站。

`BusLink` 跑在 Qt 线程里，负责收发并把帧通过信号送回界面；
`Simulator` 是一个纯线程的假电机，只用于没有硬件时演示/自测。
"""

from __future__ import annotations

import queue
import threading
import time

import can
from PySide6 import QtCore

from can_backends import open_bus
from joint_protocols import Dm4310, Feedback, Rs05


class BusLink(QtCore.QThread):
    """收发线程：recv 在子线程，发送走队列，避免跨线程碰同一个 Bus。"""

    received = QtCore.Signal(int, bytes, bool)
    opened = QtCore.Signal(bool, str)
    failed = QtCore.Signal(str)

    def __init__(self, interface: str, channel: str, bitrate: int, parent=None) -> None:
        super().__init__(parent)
        self._interface = interface
        self._channel = channel
        self._bitrate = bitrate
        self._tx: queue.Queue[tuple[int, bytes, bool]] = queue.Queue()
        self._running = True

    def send(self, arbitration_id: int, data: bytes, extended: bool) -> None:
        self._tx.put((int(arbitration_id), bytes(data), bool(extended)))

    def stop(self) -> None:
        self._running = False
        self.wait(2000)

    def _drain_tx(self, bus) -> None:
        while True:
            try:
                arb, data, ext = self._tx.get_nowait()
            except queue.Empty:
                return
            try:
                bus.send(
                    can.Message(
                        arbitration_id=arb,
                        data=data,
                        is_extended_id=ext,
                        is_fd=False,
                    )
                )
            except Exception as exc:  # noqa: BLE001 - 后端异常要显示到界面
                self.failed.emit(f"发送失败: {exc}")

    def run(self) -> None:  # noqa: D401 - QThread 入口
        try:
            bus = open_bus(self._interface, self._channel, self._bitrate)
        except Exception as exc:  # noqa: BLE001
            self.opened.emit(False, str(exc))
            return

        self.opened.emit(True, "")
        try:
            while self._running:
                self._drain_tx(bus)
                try:
                    msg = bus.recv(0.002)
                except Exception as exc:  # noqa: BLE001
                    self.failed.emit(f"接收失败: {exc}")
                    break
                if (msg is not None and not msg.is_remote_frame and not msg.is_error_frame
                        and not msg.is_fd and msg.dlc == len(msg.data)):
                    self.received.emit(
                        int(msg.arbitration_id), bytes(msg.data), bool(msg.is_extended_id)
                    )
        finally:
            try:
                bus.shutdown()
            except Exception:  # noqa: BLE001
                pass


class Simulator(threading.Thread):
    """假电机：按 MIT 阻抗律积分，1 kHz 更新、200 Hz 回反馈。

    只在 `virtual` 接口下有意义：和界面各开一个 Bus，走 python-can 的进程内队列。
    """

    def __init__(
        self,
        interface: str,
        channel: str,
        bitrate: int,
        protocol: Rs05 | Dm4310,
        node_id: int,
        host_id: int,
    ) -> None:
        super().__init__(daemon=True)
        self._interface = interface
        self._channel = channel
        self._bitrate = bitrate
        self._proto = protocol
        self._node = node_id & 0xFF
        self._host = host_id & 0xFF
        self._running = True
        self._lock = threading.Lock()
        # 简易电机模型
        self._pos = 0.0
        self._vel = 0.0
        self._tau = 0.0
        self._cmd = None
        self._armed = False
        self._fault = 0
        self._status = 0
        self._inertia = 0.02
        self._viscous = 0.02
        self._temp = 31.0

    def stop(self) -> None:
        self._running = False

    # ---- 协议事件 ----

    def _on_frame(self, msg: can.Message) -> bool:
        proto = self._proto
        data = bytes(msg.data)
        arb = int(msg.arbitration_id)

        if isinstance(proto, Rs05):
            if not msg.is_extended_id:
                return False
            ctype = (arb >> 24) & 0x1F
            if (arb & 0xFF) != self._node:
                return False
            if ctype == 0:
                self._send_ping()
                return True
            if ctype == 1:
                parsed = proto.parse_control(arb, data)
                if parsed is None:
                    return False
                self._cmd = parsed[1]
                return True
            if ctype == 3:
                self._host = (arb >> 8) & 0xFF
                self._armed = True
                self._send_feedback()
                return True
            if ctype == 4:
                self._armed = False
                self._cmd = None
                self._vel = 0.0
                if data and data[0] == 1:
                    self._fault = 0
                self._send_feedback()
                return True
            if ctype == 6:
                self._pos = 0.0
                self._vel = 0.0
                self._send_feedback()
                return True
            return False

        # DM4310：标准帧，ID = 电机 ID
        if msg.is_extended_id or arb != self._node:
            return False
        if data[:7] == b"\xff" * 7:
            code = data[7] if len(data) > 7 else 0
            if code == 0xFC:
                self._armed = True
            elif code == 0xFD:
                self._armed = False
                self._cmd = None
                self._vel = 0.0
            elif code == 0xFE:
                self._pos = 0.0
                self._vel = 0.0
            elif code == 0xFB:
                self._fault = 0
            self._send_feedback()
            return True
        parsed = proto.parse_control(arb, data)
        if parsed is not None:
            self._cmd = parsed[1]
        return False

    def _send_ping(self) -> None:
        eid = ((self._node & 0xFF) << 8) | 0xFE
        payload = bytes([1, 4, self._node, 0, 0, 0, 0, 0])
        self._send(eid, payload, True)

    def _send_feedback(self) -> None:
        proto = self._proto
        if isinstance(proto, Rs05):
            period = proto.feedback.p_max
            wrapped = (self._pos + period) % (2 * period) - period
            fb = Feedback(
                pos=wrapped,
                vel=self._vel,
                tau=self._tau,
                temp_c=self._temp,
                fault=self._fault,
                mode=2 if self._armed else 0,
            )
            arb, data = proto.make_feedback(self._node, self._host, fb)
            self._send(arb, data, True)
        else:
            period = proto.feedback.p_max
            wrapped = (self._pos + period) % (2 * period) - period
            status = 1 if self._armed else 0
            if self._fault:
                status = self._fault
            fb = Feedback(
                pos=wrapped,
                vel=self._vel,
                tau=self._tau,
                temp_c=self._temp,
                status=status,
            )
            arb, data = proto.make_feedback(self._node, self._host, fb)
            self._send(arb, data, False)

    def _send(self, arb: int, data: bytes, ext: bool) -> None:
        try:
            self._bus.send(
                can.Message(arbitration_id=arb, data=data, is_extended_id=ext, is_fd=False)
            )
        except Exception:  # noqa: BLE001 - 模拟器丢帧不影响界面
            pass

    # ---- 线程入口 ----

    def run(self) -> None:  # noqa: D401
        try:
            self._bus = open_bus(self._interface, self._channel, self._bitrate)
        except Exception:  # noqa: BLE001
            return

        rng = self._proto.control
        last = time.monotonic()
        fb_at = last
        try:
            while self._running:
                msg = self._bus.recv(0.001)
                now = time.monotonic()
                dt = min(max(now - last, 1e-4), 0.02)
                last = now

                if msg is not None:
                    with self._lock:
                        self._on_frame(msg)

                with self._lock:
                    cmd = self._cmd
                    if self._armed and cmd is not None:
                        tau = (
                            cmd.kp * (cmd.pos - self._pos)
                            + cmd.kd * (cmd.vel - self._vel)
                            + cmd.tau
                        )
                        tau = max(-rng.t_max, min(rng.t_max, tau))
                        acc = (tau - self._viscous * self._vel) / self._inertia
                        self._vel = max(-rng.v_max, min(rng.v_max, self._vel + acc * dt))
                        self._pos += self._vel * dt
                        self._tau = tau
                    else:
                        self._tau = 0.0
                        self._vel *= 1.0 - min(1.0, 2.0 * dt)

                if now - fb_at >= 0.005:
                    fb_at = now
                    with self._lock:
                        self._send_feedback()
        finally:
            try:
                self._bus.shutdown()
            except Exception:  # noqa: BLE001
                pass


__all__ = ["BusLink", "Simulator"]
