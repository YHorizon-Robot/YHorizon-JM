# YHorizon-JM

[中文](README.md) | [English](README.en.md)

This protocol-compatibility branch provides native Motor Protocol V1.0 firmware for the YHorizon-JM actuator and bench images for RS05, CyberGear, DM4310 and other protocols. It includes firmware 1.6, Rust host 0.3.0 and communication SDK 0.1.0. [Choose a host for your firmware](host-rs/README.md) (Chinese).

> **This project is published under open-source licenses.**
> Mechanical, electronics, firmware, and SDK sources are all published.
> GPLv3 and CERN-OHL-S allow commercial use when their obligations are met. For closed-source use, take a separate [commercial license](#commercial-cooperation).
> It does not guarantee reliability or safety, and it is not a product certification or quality commitment.
> Anyone who uses this project assumes the risk of personal injury, equipment damage, and other loss. See [SECURITY.en.md](SECURITY.en.md).

## Hardware and purchasing

The main branch maintains the hardware documentation. Choose the mechanical version before ordering parts.

- [Actuator specifications and supply requirements](https://github.com/YHorizon-Robot/YHorizon-JM/blob/main/README.en.md#current-actuator)
- [Production parts and assemblies](https://github.com/YHorizon-Robot/YHorizon-JM/tree/main/mechanical/MPdrawing/cad) · [Quanzhou CNC kit parts](https://github.com/YHorizon-Robot/YHorizon-JM/tree/main/mechanical/cnc) · [Kit assembly](https://github.com/YHorizon-Robot/YHorizon-JM/tree/main/mechanical/asm)
- [Mechanical BOM, standard hardware and purchasing spreadsheet](https://github.com/YHorizon-Robot/YHorizon-JM/blob/main/mechanical/bom/README.md) (Chinese) · [Driver board BOM](https://github.com/YHorizon-Robot/YHorizon-JM/blob/main/hardware/bom/BOM_YHorizon-JM.xlsx)
- [Xianyu listings and screenshots for the kit and assembled actuator preorder](https://github.com/YHorizon-Robot/YHorizon-JM/blob/main/README.en.md#build-one)

QQ discussion group **1065885316**.

## Native protocol and multi-axis sync

JM1 loads its saved calibration and validated current PI on startup. A unit without a saved current tune uses the WK4310H board defaults. Protocol: [`sdk/protocol/Motor CAN Protocol V1.0.en.md`](sdk/protocol/Motor%20CAN%20Protocol%20V1.0.en.md).

Native firmware 1.6 supports synchronized position control for 1–16 axes over Classic CAN. [Multi-axis position control guide](https://github.com/YHorizon-Robot/YHorizon-JM/blob/main/guide/multi-axis.md) (Chinese).

## Layout

| Path | Contents |
| --- | --- |
| `firmware/` | GD32F303 firmware 1.6. Board parameters live in `firmware/Boards/YH_JM/` |
| `host-rs/` | Rust host 0.3.0 |
| `sdk/` | Communication SDK 0.1.0. `sdk/python/joint_gui.py` is still the RS05 / CyberGear / DM4310 debug UI |
| `sdk/protocol/` | CAN protocol |
| `tools/` | Setup, build, and flash scripts |

## Firmware

Requires `arm-none-eabi-gcc` plus CMake and Ninja. Output: `firmware/build/gd32f303/YHorizon-JM.elf`. In CLion, open `firmware/` and select preset `gd32f303`.

Windows:

```powershell
.\tools\setup-env.ps1
.\tools\build.ps1
.\tools\flash.ps1
```

Ubuntu:

```bash
./tools/setup-env.sh
./tools/build.sh
./tools/flash.sh
./tools/run-gui.sh
```

`setup-env.sh` installs missing packages with apt and creates `sdk/python/.venv` for the RS05 / CyberGear / DM4310 debug UI. The debug probe is CMSIS-DAP over SWD. Distro OpenOCD usually has no `gd32f30x` driver; the script falls back to `tools/openocd-gd32f303-stm32f1x.cfg` (256 KB, no `mass_erase`). The factory self-test firmware uses preset `gd32f303-factory`.

Motor parameters are in `firmware/Boards/YH_JM/board_config.h`. To switch back to the voltage inner loop, change `CFG_INNER_LOOP` in `firmware/App/config.h`.

## Host

Choose the host for the firmware on the actuator. Use the **Rust GUI or CLI** in `host-rs` with native V1 (preset `gd32f303`) for motor control, flashing, commissioning, cogging compensation and group sync. Use the **Python GUI** in `sdk/python/joint_gui.py` with the DM4310, RS05 and CyberGear bench images. For AK60-6, RMD-X, LK-MF, HighTorque, DJI C610 and C620, send and inspect frames using a CAN analyzer or `python-can`. [Connection and protocol-switching guide in Chinese](host-rs/README.md)

Build from the repository root. The Windows binary is `host-rs/target/release/yfoc-host.exe`. Run it without arguments to open the GUI, and rebuild it after switching branches.

```powershell
cargo build --release --locked --manifest-path host-rs/Cargo.toml -p yfoc-host
.\host-rs\target\release\yfoc-host.exe
.\host-rs\target\release\yfoc-host.exe --interface pcan --channel PCAN_USBBUS1 --id 1 --status
```

```bash
cargo build --release --locked --manifest-path host-rs/Cargo.toml -p yfoc-host
sudo ip link set can0 up type can bitrate 1000000
./host-rs/target/release/yfoc-host --interface socketcan --channel can0 --id 1 --status
./tools/run-gui.sh
```

On Windows, install [PCAN-Basic](https://www.peak-system.com/PCAN-Basic.239.0.html) before opening PCAN. Protocol text is in [`sdk/protocol/`](sdk/protocol/). The frame-level Chinese document matches firmware 1.6; the English protocol page is an older summary.

## SDK

The [joint-motor communication SDK](sdk/README.md) is for your own control program. Python and C++17 load one native library. Rust depends on `sdk/src/yfoc-motor-sdk` directly. Build from the repository root; the library is written to `sdk/lib`:

```powershell
python sdk/build_native.py
```

```python
from yfoc_motor import Bus

with Bus("socketcan", "can0") as bus:
    sample = bus.feedback(1)
    print(sample.position, sample.velocity)
```

On Windows use `Bus("pcan", "PCAN_USBBUS1")`. Queries, modes, disable, and `run_group` are documented in [sdk/README.md](sdk/README.md).

`joint_gui.py` is a separate window for the RS05, CyberGear, and DM4310 bench images. It does not speak Motor Protocol V1.0 and it is not the Rust host in `host-rs`. Launch commands, host IDs, and the operating sequence are in [Python GUI](host-rs/README.md#python-gui-联调) (Chinese). AK60-6, RMD-X, LK-MF, HighTorque, and DJI C610/C620 are not in that window; use the frame sections below. Codec test: `python -m unittest discover -s sdk/python -p test_joint_protocols.py -v`.

### DM4310 MIT bench image

Build with `.\tools\build.ps1 -Preset gd32f303-dm4310` or `./tools/build.sh --preset gd32f303-dm4310`. Calibrate and zero the unit in native V1, assign node 1–15, stop the drive and page-flash the selected ELF. Use the matching V1 image for maintenance. The bench image receives standard 1 Mbps CAN frames on the node ID and replies on `0x10 + node`. For node 1, run `python sdk/python/joint_gui.py --protocol DM4310 --node 1 --host 0x11`.

The image handles MIT motion, FC enable, FD disable, FB clear and directed read-only registers 7/8/9/10/21/22/23. For a stationary connection check, send `7FF#0100331500000000` and expect `011#0100331500004841` (PMAX = 12.5 rad). Start a zero-gain, zero-feedforward stream at 100 Hz before enabling. Enable and accepted motion commands have a 100 ms deadline; re-enable after a timeout. FE disarms without saving a zero or acknowledging success. Register writes are ignored.

Position targets use absolute output-shaft radians relative to the JM1 zero. Feedback saturates at the wire limits of ±12.5 rad, ±30 rad/s and ±10 Nm. The bench cap is ±1 A on total Iq demand including cogging compensation. Torque feedback is estimated from measured Iq, D6 reports board temperature (0 if unavailable) and D7 is 0. A single JM1 was tested using PCAN frames and SWD checks for bridge shutdown after command timeout. Vendor-host interoperability still requires testing.

### Commission a unit before changing protocols

On a new unit, use the native JM1 CAN image (`gd32f303`) and the Rust commissioning page. With the motor secured, unloaded and free to travel, calibrate the encoder and mechanical zero, identify phase R and d/q inductance, then auto-tune current, velocity and position loops. For low-speed tracking, measure cogging in both directions and validate it. Save and read back each result. A unit with valid per-motor records can go directly to its target image using the page-flash scripts. A full-chip erase removes the records.

Both the native and third-party images load the same saved tune and cogging table. Saving new R/L invalidates the previous automatic tune and table. Saving a new PID tune requires another cogging sweep. In the current inner loop, velocity, position, impedance and output-shaft torque commands use the enabled cogging table at low speed, including active zero torque with zero gains. Direct Iq commands in amperes bypass the table. An active 0 A request slews to zero. Disable gates the bridge. The HighTorque zero-torque frame, DJI zero slot and RMD-X zero A1 retain their bench arming and gating behavior. RTT reports the stored generation and flags at boot. The stall torque diagnostic uses the saved phase resistance when available.

### Other CAN bench profiles

Choose `gd32f303-<profile>` in the build and flash scripts. All profiles use classic CAN at 1 Mbps, a 100 ms command deadline and a ±1 A total Iq demand cap. Calibrate, set the node ID and save mechanical zero in V1. Stop the drive and the old host's transmit stream before page-flashing another image. Some families share wire formats with different scales. Use a matching host on the bus. A fresh handshake is required after timeout or RX loss.

| Profile | Supported bench commands | Initial connection check for node 1 |
| --- | --- | --- |
| `rs05`, `cybergear` | Fixed host 0xFD, nodes 1–63. Types 3/1/4 enable, command and stop/clear. Type 6 sets a stationary, disabled RAM zero. Type 17 reads 7005, 7019, 701B and 701C (hex). RS05 command ranges are ±12.57/33/14 and feedback ±4π/50/5.5 in rad, rad/s and Nm. CyberGear uses ±4π/30/12 both ways. | EXT `0000FD01#0000000000000000` → EXT `000001FE` with chip ID |
| `ak60-6` | AK60-6 MIT, nodes 1–63. FC enables, FD disables. ±12.5 rad, ±45 rad/s, ±15 Nm. Board temperature +40 in D6. | STD `001#FFFFFFFFFFFFFFFD` disables and returns status on STD 001 |
| `rmd-x` | V3.91, nodes 1–32. Arm with A1 zero current. A1/A2/A4 select current, speed and speed-limited absolute position. 80 and 81 gate off and require rearming. Hosts requiring active braking on 81 need a different implementation. | STD `141#9C00000000000000` → STD 241. 92 returns signed 32-bit angle in D4–7 |
| `lk-mf` | MF V2.35, nodes 1–32. 88 enables, A1/A2/A4 control, 80/81 gate off. The MF shaft is represented by the JM1 output shaft. A1 scales ±2048 to ±16.5 A, 9C/A2/A4 replies use ±33 A. | Same 9C query → STD 141. 92 returns signed 56-bit angle in D1–7 |
| `hightorque` | Classic CAN STM32 example dialect, source 0, nodes 1–63. `05 13 00 00` arms. Subsequent torque is signed LE16 at 0.01 Nm/LSB. `01 00 00` stops. Feedback uses 0.0001 turn and 0.00025 turn/s per count. Other vendor tables use different scales. | EXT `00008001#1701` → EXT `00000100#2701…` |
| `c620`, `c610` | Nodes 1–8. A zero in the addressed slot arms or removes torque. A subsequent nonzero group command drives. Rotor angle and rpm, measured current, C620 board temperature. | Listen for STD 201, nominally every 1 ms |

Tune the native speed and position loops before using A2/A4. Persistent calibration and zero stay in V1. Current, temperature and timing need hardware validation on each target host. The Python GUI accepts `--protocol CyberGear`, `RS05` or `DM4310`. Run `python tools/package-compat.py` to build the bench archive with model-named ELFs, the native rollback ELF and a SHA-256 manifest. Packaging checks all file-backed LOAD segments against the start of calibration flash at 0x0803E800. After stopping and disconnecting the old controller, use `.\tools\flash.ps1 -ElfFile <extracted-ELF>` for rollback.

## Commercial cooperation

The open-source licenses allow commercial use if copyleft derivatives stay public. The design favors learning, easy machining, and low cost, so it is a poor fit for a mass-produced product as it stands.

For integration, manufacture, or sale without releasing your modifications, contact WeChat **Yet_Latitude** for a commercial license. The template is [`COMMERCIAL-LICENSE.md`](COMMERCIAL-LICENSE.md). An unsigned template grants nothing and does not change the rights already given by the open-source licenses.

## License

Dual license, see [`LICENSE`](LICENSE). The open-source licenses allow commercial use when their obligations are met. The commercial license is a separate path and takes effect only after it is agreed in writing.

| Content | License | Notes |
| --- | --- | --- |
| `mechanical/`, `hardware/`, `images/`, and accompanying project documentation | [CERN-OHL-S-2.0](LICENSES/CERN-OHL-S-2.0.txt) | Strongly reciprocal open-hardware license. Conveyed products or modified designs must ship the design materials CERN-OHL-S requires |
| `firmware/`, `host-rs/`, `sdk/`, `tools/` | [GPLv3](LICENSES/GPL-3.0.txt) | Commercial use is allowed; distributed derivative software must include complete corresponding source under GPLv3 |
| Separate agreement | [Commercial license template](COMMERCIAL-LICENSE.md) | For closed-source use that does not follow the obligations above. Contact WeChat Yet_Latitude; nothing is granted before signature |

## Safety

Full disclaimer: [SECURITY.en.md](SECURITY.en.md). A joint motor can produce high torque. Do not run at full power near people until limits, over-current protection, and e-stop have been verified.

## Contributing

See [CONTRIBUTING.en.md](CONTRIBUTING.en.md).
