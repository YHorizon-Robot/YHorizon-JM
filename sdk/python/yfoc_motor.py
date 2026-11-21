"""YHorizon-JM joint motor SDK. Output-shaft position and speed use rad and rad/s."""
import ctypes as C
from dataclasses import dataclass
from enum import IntEnum
import operator
import os
from pathlib import Path
import sys
from typing import Optional


class MotorError(RuntimeError):
    pass


class Mode(IntEnum):
    MOTION = 0
    VELOCITY = 1
    POSITION = 2


class InnerLoop(IntEnum):
    VOLTAGE = 0
    CURRENT = 1


@dataclass(frozen=True)
class Axis:
    node: int
    target: float
    second: Optional[float] = None


U32, I32, U64, D = C.c_uint32, C.c_int32, C.c_uint64, C.c_double


def _fields(names, typ):
    return [(name, typ) for name in names.split()]


class Info(C.Structure):
    _fields_ = _fields("board_id caps fw_major fw_minor", U32)


class Status(C.Structure):
    _fields_ = (_fields("state fault warn mode", U32) + [("inner_loop", I32)]
                + _fields("vbus temp", D))


class Feedback(C.Structure):
    _fields_ = _fields("position velocity torque", D)


class Current(C.Structure):
    _fields_ = _fields("id iq iq_ref vq", D)


class CurrentConfig(C.Structure):
    _fields_ = _fields("i_limit v_limit kp ki kt gear", D) + _fields("bandwidth_hz loop_hz", U32)


class _Target(C.Structure):
    _fields_ = _fields("node has_second", I32) + _fields("target second", D)


class Sample(C.Structure):
    _fields_ = [("host_us", U64)] + _fields("cycle node", U32) + _fields("reference position velocity", D)


_Cycle = C.CFUNCTYPE(I32, C.c_void_p, C.POINTER(Sample), C.c_size_t)


def _load():
    name = {"win32": "yfoc_motor_sdk.dll", "darwin": "libyfoc_motor_sdk.dylib"}.get(
        sys.platform, "libyfoc_motor_sdk.so")
    default = Path(__file__).resolve().parents[1] / "lib" / name
    path = os.environ.get("YFOC_SDK_LIBRARY", default)
    lib = C.CDLL(str(path))
    lib.yfoc_abi_version.argtypes = []
    lib.yfoc_abi_version.restype = U32
    if lib.yfoc_abi_version() != 1:
        raise MotorError("YFOC SDK ABI mismatch")
    lib.yfoc_last_error.argtypes = []
    lib.yfoc_last_error.restype = C.c_char_p
    signatures = {
        "open": [C.c_char_p, C.c_char_p, U32, C.POINTER(U64)],
        "close": [U64], "cancel_group": [U64],
        "set_mode": [U64, I32, I32], "set_inner": [U64, I32, I32],
        "disable": [U64, I32], "set_position": [U64, I32, D, D],
        "set_velocity": [U64, I32, D], "motion_setup": [U64, I32, D, D],
        "set_motion": [U64, I32, D, D, D],
        "wait_feedback": [U64, I32, U32, C.POINTER(Feedback)],
        "run_group": [U64, C.POINTER(_Target), C.c_size_t, D, _Cycle, C.c_void_p],
    }
    for key, typ in [("info", Info), ("status", Status), ("feedback", Feedback),
                     ("current", Current), ("current_config", CurrentConfig)]:
        signatures[key] = [U64, I32, C.POINTER(typ)]
    for key, args in signatures.items():
        fn = getattr(lib, "yfoc_" + key)
        fn.argtypes = args
        fn.restype = I32
    return lib


def _node(n):
    n = operator.index(n)
    if not 1 <= n <= 63:
        raise ValueError("node must be 1..63")
    return n


def _uint32(value):
    value = operator.index(value)
    if not 0 <= value < 2**32:
        raise ValueError("expected an unsigned 32-bit integer")
    return value


class Bus:
    """One CAN connection shared by all node IDs. Use as a context manager."""

    def __init__(self, interface, channel, bitrate=1_000_000):
        self._handle = U64()
        self._lib = _load()
        if "\0" in interface or "\0" in channel:
            raise ValueError("CAN interface and channel must not contain NUL")
        self._check(self._lib.yfoc_open(interface.encode(), channel.encode(), _uint32(bitrate),
                                        C.byref(self._handle)))

    def _check(self, rc):
        if rc < 0:
            raise MotorError(self._lib.yfoc_last_error().decode("utf-8"))
        return rc

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        if self._handle.value:
            self._check(self._lib.yfoc_close(self._handle))
            self._handle.value = 0

    def __del__(self):
        if getattr(getattr(self, "_handle", None), "value", 0):
            self._lib.yfoc_close(self._handle)

    def _read(self, fn, typ, node):
        result = typ()
        self._check(fn(self._handle, _node(node), C.byref(result)))
        return result

    def info(self, node):
        return self._read(self._lib.yfoc_info, Info, node)

    def status(self, node):
        return self._read(self._lib.yfoc_status, Status, node)

    def feedback(self, node):
        return self._read(self._lib.yfoc_feedback, Feedback, node)

    def wait_feedback(self, node, timeout_ms):
        result = Feedback()
        rc = self._check(self._lib.yfoc_wait_feedback(self._handle, _node(node),
                                                      _uint32(timeout_ms), C.byref(result)))
        return None if rc == 1 else result

    def current(self, node):
        return self._read(self._lib.yfoc_current, Current, node)

    def current_config(self, node):
        return self._read(self._lib.yfoc_current_config, CurrentConfig, node)

    def set_mode(self, node, mode):
        self._check(self._lib.yfoc_set_mode(self._handle, _node(node), Mode(mode)))

    def set_inner(self, node, inner):
        self._check(self._lib.yfoc_set_inner(self._handle, _node(node), InnerLoop(inner)))

    def set_position(self, node, rad, max_rad_s):
        self._check(self._lib.yfoc_set_position(self._handle, _node(node), rad, max_rad_s))

    def set_velocity(self, node, rad_s):
        self._check(self._lib.yfoc_set_velocity(self._handle, _node(node), rad_s))

    def motion_setup(self, node, kp, kd):
        self._check(self._lib.yfoc_motion_setup(self._handle, _node(node), kp, kd))

    def set_motion(self, node, rad, rad_s, torque_nm):
        self._check(self._lib.yfoc_set_motion(self._handle, _node(node), rad, rad_s, torque_nm))

    def disable(self, node):
        self._check(self._lib.yfoc_disable(self._handle, _node(node)))

    def cancel_group(self):
        self._check(self._lib.yfoc_cancel_group(self._handle))

    def run_group(self, axes, seconds, on_cycle=None):
        """Block until stopped. on_cycle(tuple[Sample]) may return False to cancel."""
        axes = tuple(axes)
        if not 1 <= len(axes) <= 16:
            raise ValueError("expected 1..16 axes")
        targets = (_Target * len(axes))(*(
            _Target(_node(a.node), int(a.second is not None), a.target,
                    0.0 if a.second is None else a.second) for a in axes))
        errors = []

        def cycle(_ctx, ptr, count):
            try:
                row = tuple(Sample.from_buffer_copy(ptr[i]) for i in range(count))
                return int(on_cycle(row) is False)
            except BaseException as exc:
                errors.append(exc)
                return 1

        callback = _Cycle() if on_cycle is None else _Cycle(cycle)
        rc = self._lib.yfoc_run_group(self._handle, targets, len(targets), seconds, callback, None)
        if errors:
            raise errors[0]
        self._check(rc)
