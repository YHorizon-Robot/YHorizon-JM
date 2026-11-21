"""RS05 / DM4310 关节协议编解码。

纯 Python、不依赖 CAN 与 GUI，便于单元测试和给模拟从站复用。
量纲：位置 rad、速度 rad/s、力矩 Nm、Kp Nm/rad、Kd Nm·s/rad、温度 ℃。

两家的定标都是「有符号量线性映射到 0..2^n-1」，且都是大端：
    u = (x - lo) * (2^n - 1) / (hi - lo)
与固件端 `float_to_uint` 一致，按截断取整。
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass


# ---------------------------------------------------------------- 基础换算


def f2u(x: float, lo: float, hi: float, bits: int) -> int:
    """物理量 → 无符号整数，截断取整（与厂商示例代码一致）。"""
    if hi <= lo:
        return 0
    if not math.isfinite(x):
        x = lo
    x = min(max(x, lo), hi)
    return int((x - lo) * ((1 << bits) - 1) / (hi - lo))


def u2f(u: int, lo: float, hi: float, bits: int) -> float:
    """无符号整数 → 物理量。"""
    return lo + u * (hi - lo) / ((1 << bits) - 1)


def f2u16_be(x: float, lo: float, hi: float) -> bytes:
    return struct.pack(">H", f2u(x, lo, hi, 16))


def clamp(x: float, lo: float, hi: float) -> float:
    return min(max(x, lo), hi)


# ---------------------------------------------------------------- 数据模型


@dataclass(frozen=True)
class Ranges:
    """一帧的定标量程，正负对称。"""

    p_max: float
    v_max: float
    t_max: float
    kp_max: float = 500.0
    kd_max: float = 5.0


@dataclass
class Command:
    pos: float = 0.0
    vel: float = 0.0
    kp: float = 0.0
    kd: float = 0.0
    tau: float = 0.0


@dataclass
class Feedback:
    pos: float = 0.0
    vel: float = 0.0
    tau: float = 0.0
    temp_c: float = 0.0
    fault: int = 0  # RS05 的 6 位故障字段
    mode: int = 0  # RS05 的模式：0 Reset / 1 Cali / 2 Run
    status: int = 0  # DM 的 4 位状态码
    motor_id: int = 0
    raw: bytes = b""


# ---------------------------------------------------------------- RS05

RS05_CONTROL = Ranges(p_max=12.57, v_max=33.0, t_max=14.0)
RS05_FEEDBACK = Ranges(p_max=12.566370614, v_max=50.0, t_max=5.5)

RS05_TYPE_PING = 0
RS05_TYPE_CONTROL = 1
RS05_TYPE_FEEDBACK = 2
RS05_TYPE_ENABLE = 3
RS05_TYPE_STOP = 4
RS05_TYPE_SET_ZERO = 6

RS05_FAULTS = {
    0: "欠压",
    1: "过流",
    2: "过温",
    3: "编码器",
    4: "堵转/过载",
    5: "未标定",
}

RS05_MODES = {0: "Reset", 1: "Cali", 2: "Run"}


class Rs05:
    """灵足时代 RS05 私有协议（29 位扩展帧）。"""

    name = "RS05"
    extended = True
    control = RS05_CONTROL
    feedback = RS05_FEEDBACK

    @staticmethod
    def _id(comm_type: int, node: int, host: int) -> int:
        return ((comm_type & 0x1F) << 24) | ((host & 0xFF) << 8) | (node & 0xFF)

    def make_control(self, node: int, host: int, cmd: Command) -> tuple[int, bytes]:
        r = self.control
        tau_u = f2u(cmd.tau, -r.t_max, r.t_max, 16)
        eid = (RS05_TYPE_CONTROL << 24) | (tau_u << 8) | (node & 0xFF)
        data = (
            f2u16_be(cmd.pos, -r.p_max, r.p_max)
            + f2u16_be(cmd.vel, -r.v_max, r.v_max)
            + f2u16_be(cmd.kp, 0.0, r.kp_max)
            + f2u16_be(cmd.kd, 0.0, r.kd_max)
        )
        return eid, data

    def make_enable(self, node: int, host: int) -> tuple[int, bytes]:
        return self._id(RS05_TYPE_ENABLE, node, host), bytes(8)

    def make_disable(self, node: int, host: int, clear_fault: bool = False) -> tuple[int, bytes]:
        data = bytes([1 if clear_fault else 0] + [0] * 7)
        return self._id(RS05_TYPE_STOP, node, host), data

    def make_set_zero(self, node: int, host: int) -> tuple[int, bytes]:
        return self._id(RS05_TYPE_SET_ZERO, node, host), bytes([1] + [0] * 7)

    def make_ping(self, node: int, host: int) -> tuple[int, bytes]:
        return self._id(RS05_TYPE_PING, node, host), bytes(8)

    ## 反馈方向

    def parse_feedback(self, arb: int, data: bytes) -> Feedback | None:
        if ((arb >> 24) & 0x1F) != RS05_TYPE_FEEDBACK or len(data) < 8:
            return None
        pos_u, vel_u, tau_u, temp_u = struct.unpack(">HHHH", data[:8])
        r = self.feedback
        return Feedback(
            pos=u2f(pos_u, -r.p_max, r.p_max, 16),
            vel=u2f(vel_u, -r.v_max, r.v_max, 16),
            tau=u2f(tau_u, -r.t_max, r.t_max, 16),
            temp_c=temp_u / 10.0,
            fault=(arb >> 16) & 0x3F,
            mode=(arb >> 22) & 0x03,
            motor_id=(arb >> 8) & 0xFF,
            raw=bytes(data[:8]),
        )

    def make_feedback(self, node: int, host: int, fb: Feedback) -> tuple[int, bytes]:
        r = self.feedback
        fm = (fb.fault & 0x3F) | ((fb.mode & 0x03) << 6)
        eid = (RS05_TYPE_FEEDBACK << 24) | (fm << 16) | ((node & 0xFF) << 8) | (host & 0xFF)
        data = (
            f2u16_be(fb.pos, -r.p_max, r.p_max)
            + f2u16_be(fb.vel, -r.v_max, r.v_max)
            + f2u16_be(fb.tau, -r.t_max, r.t_max)
            + struct.pack(">H", int(clamp(fb.temp_c * 10.0, -32768, 65535)) & 0xFFFF)
        )
        return eid, data

    def parse_control(self, arb: int, data: bytes) -> tuple[int, Command] | None:
        """给模拟从站用：从控制帧解出 (电机 ID, 目标量)。"""
        if ((arb >> 24) & 0x1F) != RS05_TYPE_CONTROL or len(data) < 8:
            return None
        r = self.control
        pos_u, vel_u, kp_u, kd_u = struct.unpack(">HHHH", data[:8])
        cmd = Command(
            pos=u2f(pos_u, -r.p_max, r.p_max, 16),
            vel=u2f(vel_u, -r.v_max, r.v_max, 16),
            kp=u2f(kp_u, 0.0, r.kp_max, 16),
            kd=u2f(kd_u, 0.0, r.kd_max, 16),
            tau=u2f((arb >> 8) & 0xFFFF, -r.t_max, r.t_max, 16),
        )
        return (arb & 0xFF, cmd)


# ---------------------------------------------------------------- DM4310

DM_RANGES = Ranges(p_max=12.5, v_max=30.0, t_max=10.0)

DM_CMD_ENABLE = 0xFC
DM_CMD_DISABLE = 0xFD
DM_CMD_SET_ZERO = 0xFE
DM_CMD_CLEAR_FAULT = 0xFB

DM_STATUS = {
    0: "未使能",
    1: "已使能",
    8: "过压",
    9: "欠压",
    0xA: "过流",
    0xB: "MOS 过温",
    0xC: "转子过温",
    0xD: "通信丢失",
    0xE: "过载",
}


class Dm4310:
    """达妙 DM4310：11 位标准帧 MIT 协议。"""

    name = "DM4310"
    extended = False

    def __init__(self, ranges: Ranges | None = None) -> None:
        self.control = ranges or DM_RANGES
        self.feedback = self.control

    @staticmethod
    def _system(node: int, code: int) -> tuple[int, bytes]:
        return node & 0x7FF, bytes([0xFF] * 7 + [code])

    def make_control(self, node: int, host: int, cmd: Command) -> tuple[int, bytes]:
        r = self.control
        pos_u = f2u(cmd.pos, -r.p_max, r.p_max, 16)
        vel_u = f2u(cmd.vel, -r.v_max, r.v_max, 12)
        kp_u = f2u(cmd.kp, 0.0, r.kp_max, 12)
        kd_u = f2u(cmd.kd, 0.0, r.kd_max, 12)
        tau_u = f2u(cmd.tau, -r.t_max, r.t_max, 12)
        data = bytes(
            [
                (pos_u >> 8) & 0xFF,
                pos_u & 0xFF,
                (vel_u >> 4) & 0xFF,
                ((vel_u & 0x0F) << 4) | ((kp_u >> 8) & 0x0F),
                kp_u & 0xFF,
                (kd_u >> 4) & 0xFF,
                ((kd_u & 0x0F) << 4) | ((tau_u >> 8) & 0x0F),
                tau_u & 0xFF,
            ]
        )
        return node & 0x7FF, data

    def make_enable(self, node: int, host: int) -> tuple[int, bytes]:
        return self._system(node, DM_CMD_ENABLE)

    def make_disable(self, node: int, host: int, clear_fault: bool = False) -> tuple[int, bytes]:
        return self._system(node, DM_CMD_DISABLE)

    def make_set_zero(self, node: int, host: int) -> tuple[int, bytes]:
        return self._system(node, DM_CMD_SET_ZERO)

    def make_clear_fault(self, node: int, host: int) -> tuple[int, bytes]:
        return self._system(node, DM_CMD_CLEAR_FAULT)

    def make_ping(self, node: int, host: int) -> tuple[int, bytes] | None:
        return None

    ## 反馈方向

    def parse_feedback(self, arb: int, data: bytes) -> Feedback | None:
        if len(data) < 8:
            return None
        r = self.feedback
        pos_u = (data[1] << 8) | data[2]
        vel_u = (data[3] << 4) | (data[4] >> 4)
        tau_u = ((data[4] & 0x0F) << 8) | data[5]
        return Feedback(
            pos=u2f(pos_u, -r.p_max, r.p_max, 16),
            vel=u2f(vel_u, -r.v_max, r.v_max, 12),
            tau=u2f(tau_u, -r.t_max, r.t_max, 12),
            temp_c=float(data[6]),
            status=(data[0] >> 4) & 0x0F,
            motor_id=data[0] & 0x0F,
            raw=bytes(data[:8]),
        )

    def make_feedback(self, node: int, host: int, fb: Feedback) -> tuple[int, bytes]:
        r = self.feedback
        pos_u = f2u(fb.pos, -r.p_max, r.p_max, 16)
        vel_u = f2u(fb.vel, -r.v_max, r.v_max, 12)
        tau_u = f2u(fb.tau, -r.t_max, r.t_max, 12)
        data = bytes(
            [
                ((fb.status & 0x0F) << 4) | (node & 0x0F),
                (pos_u >> 8) & 0xFF,
                pos_u & 0xFF,
                (vel_u >> 4) & 0xFF,
                ((vel_u & 0x0F) << 4) | ((tau_u >> 8) & 0x0F),
                tau_u & 0xFF,
                int(clamp(fb.temp_c, 0, 255)),
                0,
            ]
        )
        return host & 0x7FF, data

    def parse_control(self, arb: int, data: bytes) -> tuple[int, Command] | None:
        if len(data) < 8 or (arb < 0 or arb > 0x7FF):
            return None
        if data[:7] == b"\xff" * 7:
            return None
        r = self.control
        pos_u = (data[0] << 8) | data[1]
        vel_u = (data[2] << 4) | (data[3] >> 4)
        kp_u = ((data[3] & 0x0F) << 8) | data[4]
        kd_u = (data[5] << 4) | (data[6] >> 4)
        tau_u = ((data[6] & 0x0F) << 8) | data[7]
        cmd = Command(
            pos=u2f(pos_u, -r.p_max, r.p_max, 16),
            vel=u2f(vel_u, -r.v_max, r.v_max, 12),
            kp=u2f(kp_u, 0.0, r.kp_max, 12),
            kd=u2f(kd_u, 0.0, r.kd_max, 12),
            tau=u2f(tau_u, -r.t_max, r.t_max, 12),
        )
        return arb & 0x7FF, cmd


def protocol_by_name(name: str) -> Rs05 | Dm4310:
    return Dm4310() if name.upper().startswith("DM") else Rs05()


def describe_fault(proto: Rs05 | Dm4310, fb: Feedback) -> str:
    """把反馈里的故障/状态翻译成中文，正常时返回空串。"""
    if isinstance(proto, Rs05):
        bits = [text for bit, text in RS05_FAULTS.items() if fb.fault & (1 << bit)]
        return "、".join(bits)
    if fb.status in (0, 1):
        return ""
    return DM_STATUS.get(fb.status, f"未知状态 0x{fb.status:X}")
