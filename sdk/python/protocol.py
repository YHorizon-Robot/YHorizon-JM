"""Motor CAN Protocol V1.0 pack/unpack helpers."""

from __future__ import annotations

import struct

POS_LSB = 0.0001
VEL_LSB = 0.1
KP_LSB_MOTION = 0.01
KD_LSB_MOTION = 0.001
KP_LSB_VEL = 0.001
KI_LSB_VEL = 0.001
KP_LSB_POS = 0.01
KI_LSB_POS = 0.001
KD_LSB_POS = 0.001
TORQUE_LSB = 0.001
VBUS_LSB = 0.01
TEMP_LSB = 0.1

INNER_LOOP_VOLTAGE = 0
INNER_LOOP_CURRENT = 1
INNER_LOOP_NAME = {INNER_LOOP_VOLTAGE: "VOLTAGE", INNER_LOOP_CURRENT: "CURRENT"}
# Firmware CFG_I_LIMIT_A / CFG_V_LIMIT: current loop maps t_ref (pu) to Iq_ref (A).
CURRENT_A_PER_PU = 5.0 / 0.5

MODE_MOTION = 0x00
MODE_VELOCITY = 0x01
MODE_POSITION = 0x02

CMD_ENABLE = 0x01
CMD_DISABLE = 0x02
CMD_SET_ZERO = 0x03
CMD_CLEAR_FAULT = 0x04
CMD_START_CALI = 0x05
CMD_SET_MODE = 0x06
CMD_SET_NODE_ID = 0x07
CMD_SAVE_USER_PARAMS = 0x08
CMD_GET_STATUS = 0x10
CMD_SET_GAINS = 0x20
CMD_GET_GAINS = 0x21

CAN_MOTION = 0x100
CAN_VEL = 0x140
CAN_GAINS = 0x180
CAN_POS = 0x1C0
CAN_MGMT = 0x200
CAN_ACK = 0x280
CAN_FB = 0x300
CAN_STATUS = 0x380
CAN_CALI = 0x3C0

MODE_NAME = {
    MODE_MOTION: "MOTION",
    MODE_VELOCITY: "VELOCITY",
    MODE_POSITION: "POSITION",
}

STATE_NAME = {
    0x00: "DISABLED",
    0x01: "READY",
    0x02: "RUNNING",
    0x03: "FAULT",
    0x04: "CALIBRATING",
}

RESULT_NAME = {
    0x00: "OK",
    0x01: "INVALID_COMMAND",
    0x02: "INVALID_STATE",
    0x03: "PARAMETER_OUT_OF_RANGE",
    0x04: "FAULT_ACTIVE",
    0x05: "BUSY",
    0x06: "INTERNAL_ERROR",
}

CMD_NAME = {
    CMD_ENABLE: "ENABLE",
    CMD_DISABLE: "DISABLE",
    CMD_SET_ZERO: "SET_ZERO",
    CMD_CLEAR_FAULT: "CLEAR_FAULT",
    CMD_START_CALI: "START_CALI",
    CMD_SET_MODE: "SET_CONTROL_MODE",
    CMD_SET_NODE_ID: "SET_NODE_ID",
    CMD_SAVE_USER_PARAMS: "SAVE_USER_PARAMS",
    CMD_GET_STATUS: "GET_STATUS",
    CMD_SET_GAINS: "SET_GAINS",
    CMD_GET_GAINS: "GET_GAINS",
}

CALI_ERROR_NAME = {
    0x0000: "NO_ERROR",
    0x0001: "INVALID_STATE",
    0x0002: "ENCODER_SIGNAL_ERROR",
    0x0003: "CALIBRATION_TIMEOUT",
    0x0004: "CALIBRATION_DATA_INVALID",
    0x0005: "PARAMETER_SAVE_FAILED",
    0x0006: "MOTOR_CONTROL_ERROR",
    0x00FF: "INTERNAL_ERROR",
}
CALI_STATE_NAME = {0x00: "IDLE", 0x01: "RUNNING", 0x02: "SUCCESS", 0x03: "FAILED"}
CALI_STAGE_NAME = {
    0x00: "IDLE",
    0x01: "PRE_CHECK",
    0x02: "MOTOR_ALIGNMENT",
    0x03: "ENCODER_SAMPLING",
    0x04: "PARAMETER_CALCULATION",
    0x05: "PARAMETER_SAVE",
    0x06: "COMPLETE",
}

FAULT_BITS = (
    (0, "OVER_TEMPERATURE"),
    (1, "OVER_CURRENT"),
    (2, "OVER_VOLTAGE"),
    (3, "UNDER_VOLTAGE"),
    (4, "ENCODER_FAULT"),
    (5, "STALL_OVERLOAD"),
    (6, "DRIVER_FAULT"),
    (8, "POSITION_ERROR"),
    (9, "MOTOR_PHASE_FAULT"),
    (10, "CALIBRATION_FAULT"),
)


def ids(node_id: int) -> dict[str, int]:
    nid = max(1, min(63, int(node_id)))
    return {
        "motion": CAN_MOTION + nid,
        "vel": CAN_VEL + nid,
        "gains": CAN_GAINS + nid,
        "pos": CAN_POS + nid,
        "mgmt": CAN_MGMT + nid,
        "ack": CAN_ACK + nid,
        "fb": CAN_FB + nid,
        "status": CAN_STATUS + nid,
        "cali": CAN_CALI + nid,
    }


def clamp_i16(v: int) -> int:
    return max(-32768, min(32767, int(v)))


def clamp_u16(v: int) -> int:
    return max(0, min(65535, int(v)))


def pack_motion(pos: float, vel: float, vff: float) -> bytes:
    pos_raw = int(round(pos / POS_LSB))
    vel_raw = clamp_i16(int(round(vel / VEL_LSB)))
    vff = min(max(float(vff), -1.0), 1.0)
    ff_raw = clamp_i16(int(round(vff * 32767.0)))
    return struct.pack("<ihh", pos_raw, vel_raw, ff_raw)


def pack_velocity(vel: float) -> bytes:
    vel_raw = int(round(vel / VEL_LSB))
    return struct.pack("<iI", vel_raw, 0)


def pack_position(pos: float, max_vel: float) -> bytes:
    pos_raw = int(round(pos / POS_LSB))
    max_raw = max(0, int(round(abs(max_vel) / VEL_LSB)))
    return struct.pack("<iI", pos_raw, max_raw)


def pack_gains(mode: int, seq: int, kp: float, ki: float, kd: float) -> bytes:
    if mode == MODE_MOTION:
        kp_raw = clamp_u16(int(round(kp / KP_LSB_MOTION)))
        ki_raw = 0
        kd_raw = clamp_u16(int(round(kd / KD_LSB_MOTION)))
    elif mode == MODE_VELOCITY:
        kp_raw = clamp_u16(int(round(kp / KP_LSB_VEL)))
        ki_raw = clamp_u16(int(round(ki / KI_LSB_VEL)))
        kd_raw = 0
    else:
        kp_raw = clamp_u16(int(round(kp / KP_LSB_POS)))
        ki_raw = clamp_u16(int(round(ki / KI_LSB_POS)))
        kd_raw = clamp_u16(int(round(kd / KD_LSB_POS)))
    return struct.pack("<BBHHH", mode & 0xFF, seq & 0xFF, kp_raw, ki_raw, kd_raw)


def unpack_gains(data: bytes) -> dict[str, float | int | str]:
    mode, seq, kp_raw, ki_raw, kd_raw = struct.unpack("<BBHHH", data)
    if mode == MODE_MOTION:
        kp = kp_raw * KP_LSB_MOTION
        ki = 0.0
        kd = kd_raw * KD_LSB_MOTION
    elif mode == MODE_VELOCITY:
        kp = kp_raw * KP_LSB_VEL
        ki = ki_raw * KI_LSB_VEL
        kd = 0.0
    else:
        kp = kp_raw * KP_LSB_POS
        ki = ki_raw * KI_LSB_POS
        kd = kd_raw * KD_LSB_POS
    return {
        "mode": mode,
        "mode_name": MODE_NAME.get(mode, f"0x{mode:02X}"),
        "seq": seq,
        "kp": kp,
        "ki": ki,
        "kd": kd,
    }


def pack_mgmt(cmd: int, seq: int, arg: int = 0) -> bytes:
    data = bytearray(8)
    data[0] = cmd & 0xFF
    data[1] = seq & 0xFF
    data[2] = arg & 0xFF
    return bytes(data)


def unpack_feedback(data: bytes) -> dict[str, float]:
    pos_raw, vel_raw, torque_raw = struct.unpack("<ihh", data)
    return {
        "pos": pos_raw * POS_LSB,
        "vel": vel_raw * VEL_LSB,
        "torque": torque_raw * TORQUE_LSB,
    }


def unpack_ack(data: bytes) -> dict[str, int | str]:
    cmd = data[0]
    result = data[2]
    state = data[3]
    fault = int.from_bytes(data[4:6], "little")
    return {
        "cmd": cmd,
        "cmd_name": CMD_NAME.get(cmd, f"0x{cmd:02X}"),
        "seq": data[1],
        "result": result,
        "result_name": RESULT_NAME.get(result, f"0x{result:02X}"),
        "state": state,
        "state_name": STATE_NAME.get(state, f"0x{state:02X}"),
        "fault": fault,
        "fault_name": fault_name(fault),
        "new_id": data[6],
    }


def unpack_status(data: bytes) -> dict[str, float | int | str]:
    state = data[0]
    fault = int.from_bytes(data[1:3], "little")
    vbus = int.from_bytes(data[3:5], "little") * VBUS_LSB
    temp = int.from_bytes(data[5:7], "little", signed=True) * TEMP_LSB
    byte7 = data[7]
    mode = (byte7 >> 5) & 0x03
    inner = (byte7 >> 7) & 0x01
    return {
        "state": state,
        "state_name": STATE_NAME.get(state, f"0x{state:02X}"),
        "fault": fault,
        "fault_name": fault_name(fault),
        "vbus": vbus,
        "temp": temp,
        "mode": mode,
        "mode_name": MODE_NAME.get(mode, f"0x{mode:02X}"),
        "inner_loop": inner,
        "inner_loop_name": INNER_LOOP_NAME[inner],
        "warn": byte7 & 0x1F,
    }


def torque_physical(t_ref: float, inner_loop: int | None, vbus: float | None) -> tuple[float, str] | None:
    """Map feedback t_ref (pu) to Vq in V or Iq_ref in A; None if the inner loop is unknown."""
    if inner_loop == INNER_LOOP_CURRENT:
        return t_ref * CURRENT_A_PER_PU, "A"
    if inner_loop == INNER_LOOP_VOLTAGE and vbus is not None and vbus > 0.0:
        return t_ref * vbus, "V"
    return None


def unpack_cali(data: bytes) -> dict[str, int | str]:
    state = data[1]
    stage = data[3]
    err = int.from_bytes(data[4:6], "little")
    return {
        "seq": data[0],
        "state": state,
        "state_name": CALI_STATE_NAME.get(state, f"0x{state:02X}"),
        "progress": data[2],
        "stage": stage,
        "stage_name": CALI_STAGE_NAME.get(stage, f"0x{stage:02X}"),
        "error": err,
        "error_name": CALI_ERROR_NAME.get(err, f"0x{err:04X}"),
    }


def fault_name(fault: int) -> str:
    if fault == 0:
        return "NONE"
    names = [name for bit, name in FAULT_BITS if (fault >> bit) & 1]
    return ",".join(names) if names else f"0x{fault:04X}"
