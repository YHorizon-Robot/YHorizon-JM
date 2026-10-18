#!/usr/bin/env python3
"""Minimal YHorizon-JM host CLI: Motor CAN Protocol V1.0 motion stream."""

from __future__ import annotations

import argparse
import sys
import time

try:
    import can
except ImportError:
    print("python-can is required: pip install python-can", file=sys.stderr)
    raise

import can_backends
import protocol as proto


def parse_args() -> argparse.Namespace:
    default_interface = can_backends.default_interface()
    default_channel = can_backends.default_channel(default_interface)
    parser = argparse.ArgumentParser(description="YHorizon-JM servo host")
    parser.add_argument("--interface", default=default_interface, help="python-can interface")
    parser.add_argument("--channel", default=default_channel, help="CAN channel / slcan device")
    parser.add_argument("--list-can", action="store_true", help="list detected CAN channels and exit")
    parser.add_argument("--bitrate", type=int, default=1000000)
    parser.add_argument("--id", type=int, default=1, help="motor id 1..63")
    parser.add_argument("--pos", type=float, default=0.0, help="target angle rad")
    parser.add_argument("--vel", type=float, default=0.0, help="target velocity rad/s")
    parser.add_argument("--kp", type=float, default=0.0)
    parser.add_argument("--kd", type=float, default=0.0)
    parser.add_argument("--ff", type=float, default=0.0, help="torque feedforward pu -1..1")
    parser.add_argument("--rate", type=float, default=200.0, help="motion TX rate Hz")
    parser.add_argument("--listen", action="store_true", help="only parse bus frames")
    parser.add_argument("--once", action="store_true", help="send setup + one motion frame, then exit")
    parser.add_argument("--gui", action="store_true", help="open GUI instead of CLI")
    return parser.parse_args()


def wait_ack(bus: can.BusABC, ack_id: int, cmd: int, seq: int, timeout_s: float = 0.05):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        msg = bus.recv(timeout=max(0.0, deadline - time.monotonic()))
        if msg is None or msg.is_extended_id or msg.arbitration_id != ack_id:
            continue
        ack = proto.unpack_ack(bytes(msg.data))
        if ack["cmd"] == cmd and ack["seq"] == seq:
            return ack
    return None


def main() -> int:
    args = parse_args()
    if args.gui:
        from servo_gui import main as gui_main

        return gui_main()
    if args.list_can:
        iface = args.interface
        found = can_backends.detect_channels(iface)
        print(f"interface={iface} default_channel={can_backends.default_channel(iface)}")
        print("detected: " + (", ".join(found) if found else "(none)"))
        print("choices:  " + ", ".join(can_backends.list_channels(iface)))
        return 0

    ids = proto.ids(args.id)
    bus = can_backends.open_bus(args.interface, args.channel, args.bitrate)
    motion = proto.pack_motion(args.pos, args.vel, args.ff)
    period = 0.0 if args.rate <= 0.0 else 1.0 / args.rate
    next_tx = 0.0
    seq = 1

    print(
        f"motion=0x{ids['motion']:03X} fb=0x{ids['fb']:03X} "
        f"pos={args.pos:.3f} vel={args.vel:.3f} kp={args.kp:.3f} kd={args.kd:.3f} ff={args.ff:.3f}"
    )
    try:
        if not args.listen:
            bus.send(
                can.Message(
                    arbitration_id=ids["gains"],
                    data=proto.pack_gains(proto.MODE_MOTION, seq, args.kp, 0.0, args.kd),
                    is_extended_id=False,
                )
            )
            ack = wait_ack(bus, ids["ack"], proto.CMD_SET_GAINS, seq)
            print(f"gains ack={ack['result_name'] if ack else 'timeout'}")

        while True:
            now = time.monotonic()
            if not args.listen and period > 0.0 and (next_tx == 0.0 or now >= next_tx):
                bus.send(can.Message(arbitration_id=ids["motion"], data=motion, is_extended_id=False))
                if next_tx == 0.0:
                    next_tx = now + period
                else:
                    next_tx += period
                    if next_tx < now:
                        next_tx = now + period

            leftover = (next_tx - time.monotonic()) if (not args.listen and period > 0.0) else 0.05
            msg = bus.recv(timeout=min(0.001, max(0.0, leftover)) if period > 0.0 and not args.listen else 0.05)
            if msg is None or msg.is_extended_id:
                continue
            if msg.arbitration_id == ids["fb"]:
                fb = proto.unpack_feedback(bytes(msg.data))
                print(f"fb pos={fb['pos']:+.4f} rad  vel={fb['vel']:+.3f} rad/s  t={fb['torque']:+.3f}")
                if args.once:
                    return 0
            elif msg.arbitration_id == ids["ack"]:
                ack = proto.unpack_ack(bytes(msg.data))
                print(
                    f"ack {ack['cmd_name']} seq={ack['seq']} "
                    f"result={ack['result_name']} state={ack['state_name']}"
                )
            elif msg.arbitration_id == ids["status"]:
                st = proto.unpack_status(bytes(msg.data))
                print(
                    f"status {st['state_name']} fault={st['fault_name']} mode={st['mode_name']} "
                    f"inner={st['inner_loop_name']} vbus={st['vbus']:.2f}V"
                )
    except KeyboardInterrupt:
        return 0
    finally:
        bus.shutdown()


if __name__ == "__main__":
    sys.exit(main())
