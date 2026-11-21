"""RS05 / DM4310 协议编解码单元测试。

运行：
    python -m unittest discover -s sdk/python -p test_joint_protocols.py -v
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from joint_protocols import (  # noqa: E402
    Command,
    DM_STATUS,
    Dm4310,
    Feedback,
    Rs05,
    RS05_CONTROL,
    RS05_FEEDBACK,
    describe_fault,
    f2u,
    u2f,
)


class ConversionTest(unittest.TestCase):
    def test_zero_maps_to_midpoint(self) -> None:
        self.assertEqual(f2u(0.0, -12.57, 12.57, 16), 32767)
        self.assertEqual(f2u(0.0, -30.0, 30.0, 12), 2047)
        self.assertAlmostEqual(u2f(32767, -12.57, 12.57, 16), -0.00019, places=4)

    def test_clamps_out_of_range(self) -> None:
        self.assertEqual(f2u(99.0, -12.5, 12.5, 16), 65535)
        self.assertEqual(f2u(-99.0, -12.5, 12.5, 16), 0)
        self.assertEqual(f2u(float("nan"), -12.5, 12.5, 16), 0)


class Rs05Test(unittest.TestCase):
    def setUp(self) -> None:
        self.proto = Rs05()

    def test_control_puts_torque_in_id_and_floats_in_data(self) -> None:
        cmd = Command(pos=1.0, vel=-2.0, kp=10.0, kd=0.5, tau=3.0)
        eid, data = self.proto.make_control(7, 0xFD, cmd)

        self.assertEqual(len(data), 8)
        self.assertEqual((eid >> 24) & 0x1F, 1)
        self.assertEqual(eid & 0xFF, 7)
        # 力矩编码在 bit23..8
        tau_u = (eid >> 8) & 0xFFFF
        self.assertGreater(tau_u, 32767)

        node, decoded = self.proto.parse_control(eid, data)
        self.assertEqual(node, 7)
        self.assertAlmostEqual(decoded.pos, 1.0, delta=1e-3)
        self.assertAlmostEqual(decoded.vel, -2.0, delta=1e-3)
        self.assertAlmostEqual(decoded.kp, 10.0, delta=0.05)
        self.assertAlmostEqual(decoded.kd, 0.5, delta=0.005)
        self.assertAlmostEqual(decoded.tau, 3.0, delta=1e-3)

    def test_feedback_round_trip(self) -> None:
        fb = Feedback(pos=-3.5, vel=12.0, tau=2.5, temp_c=42.5, fault=0b100010, mode=2)
        eid, data = self.proto.make_feedback(7, 0xFD, fb)

        self.assertEqual((eid >> 24) & 0x1F, 2)
        self.assertEqual((eid >> 8) & 0xFF, 7)
        self.assertEqual(eid & 0xFF, 0xFD)

        got = self.proto.parse_feedback(eid, data)
        assert got is not None
        self.assertAlmostEqual(got.pos, -3.5, delta=1e-3)
        self.assertAlmostEqual(got.vel, 12.0, delta=0.01)
        self.assertAlmostEqual(got.tau, 2.5, delta=1e-3)
        self.assertAlmostEqual(got.temp_c, 42.5, delta=0.1)
        self.assertEqual(got.fault, 0b100010)
        self.assertEqual(got.mode, 2)
        self.assertEqual(got.motor_id, 7)

    def test_feedback_rejects_control_frame(self) -> None:
        eid, data = self.proto.make_control(1, 0xFD, Command())
        self.assertIsNone(self.proto.parse_feedback(eid, data))

    def test_system_frames(self) -> None:
        eid, data = self.proto.make_enable(3, 0xFD)
        self.assertEqual((eid >> 24) & 0x1F, 3)
        self.assertEqual(eid & 0xFF, 3)
        self.assertEqual((eid >> 8) & 0xFF, 0xFD)
        self.assertEqual(data, bytes(8))

        eid, data = self.proto.make_set_zero(3, 0xFD)
        self.assertEqual((eid >> 24) & 0x1F, 6)
        self.assertEqual(data[0], 1)

        eid, data = self.proto.make_disable(3, 0xFD, clear_fault=True)
        self.assertEqual((eid >> 24) & 0x1F, 4)
        self.assertEqual(data[0], 1)

    def test_ranges_match_firmware_constants(self) -> None:
        self.assertAlmostEqual(RS05_CONTROL.p_max, 12.57, places=2)
        self.assertAlmostEqual(RS05_CONTROL.v_max, 33.0, places=2)
        self.assertAlmostEqual(RS05_CONTROL.t_max, 14.0, places=2)
        self.assertAlmostEqual(RS05_FEEDBACK.p_max, 12.566371, places=5)
        self.assertAlmostEqual(RS05_FEEDBACK.t_max, 5.5, places=3)

    def test_fault_text(self) -> None:
        fb = Feedback(fault=(1 << 0) | (1 << 3))
        self.assertEqual(describe_fault(self.proto, fb), "欠压、编码器")
        self.assertEqual(describe_fault(self.proto, Feedback(fault=0)), "")


class Dm4310Test(unittest.TestCase):
    def setUp(self) -> None:
        self.proto = Dm4310()

    def test_zero_command_bytes(self) -> None:
        arb, data = self.proto.make_control(1, 0, Command())
        self.assertEqual(arb, 1)
        # 位置/速度/力矩都是带偏置的定标量，零对应中点：
        # pos=0x7FFF, vel=0x7FF, kp=0, kd=0, tau=0x7FF
        self.assertEqual(data, bytes.fromhex("7FFF7FF0000007FF"))

    def test_mit_round_trip(self) -> None:
        cmd = Command(pos=-6.0, vel=8.0, kp=120.0, kd=1.2, tau=-4.0)
        arb, data = self.proto.make_control(5, 0x15, cmd)
        self.assertEqual(arb, 5)
        self.assertEqual(len(data), 8)

        node, got = self.proto.parse_control(arb, data)
        self.assertEqual(node, 5)
        self.assertAlmostEqual(got.pos, -6.0, delta=0.01)
        self.assertAlmostEqual(got.vel, 8.0, delta=0.02)
        self.assertAlmostEqual(got.kp, 120.0, delta=0.2)
        self.assertAlmostEqual(got.kd, 1.2, delta=0.01)
        self.assertAlmostEqual(got.tau, -4.0, delta=0.01)

    def test_system_frames_are_not_control(self) -> None:
        cases = {
            0xFC: self.proto.make_enable(5, 0),
            0xFD: self.proto.make_disable(5, 0),
            0xFE: self.proto.make_set_zero(5, 0),
            0xFB: self.proto.make_clear_fault(5, 0),
        }
        for code, (arb, data) in cases.items():
            self.assertEqual(data[:7], b"\xff" * 7)
            self.assertEqual(data[7], code)
            self.assertIsNone(self.proto.parse_control(arb, data))

    def test_feedback_layout(self) -> None:
        fb = Feedback(pos=0.0, vel=0.0, tau=0.0, temp_c=40.0, status=1)
        arb, data = self.proto.make_feedback(3, 0x13, fb)
        self.assertEqual(arb, 0x13)
        self.assertEqual(data[0], 0x13)  # status=1, motor_id=3
        self.assertEqual(data[1:3], b"\x7f\xff")
        self.assertEqual(data[6], 40)

        got = self.proto.parse_feedback(arb, data)
        assert got is not None
        self.assertEqual(got.status, 1)
        self.assertEqual(got.motor_id, 3)
        self.assertAlmostEqual(got.temp_c, 40.0, delta=0.1)
        self.assertAlmostEqual(got.pos, 0.0, delta=0.001)

    def test_status_text(self) -> None:
        self.assertEqual(describe_fault(self.proto, Feedback(status=1)), "")
        self.assertEqual(describe_fault(self.proto, Feedback(status=0x9)), DM_STATUS[9])


if __name__ == "__main__":
    unittest.main()
