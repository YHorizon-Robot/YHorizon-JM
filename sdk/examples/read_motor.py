"""Read a node without commanding motion: read_motor.py pcan PCAN_USBBUS1 1"""
import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from yfoc_motor import Bus

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("interface")
parser.add_argument("channel")
parser.add_argument("node", type=int)
args = parser.parse_args()
with Bus(args.interface, args.channel) as bus:
    info = bus.info(args.node)
    status = bus.status(args.node)
    sample = bus.feedback(args.node)
    print(f"node {args.node}  firmware {info.fw_major}.{info.fw_minor}  fault {status.fault}"
          f"  position {sample.position:.4f} rad  speed {sample.velocity:.2f} rad/s")
