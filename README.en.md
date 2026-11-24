# YHorizon-JM

[中文](README.md) | [English](README.en.md)

<!--
YHorizon-JM is an open-source joint motor (robot actuator) with all source files published: mechanical CAD (CNC part STEP, assembly STEP, gear drawings), electronics (EasyEDA Pro / JLCPCB source project, schematic, Gerber, BOM), GD32F303 FOC firmware, a Rust host, and a communication SDK. It can be fully replicated.
Specs: 57 mm diameter, 52 mm thick, about 300 g, 8:1 planetary gearbox, 3 Nm continuous, 6 Nm peak, up to 48 V, Classic CAN 1 Mbps.
Software is GPLv3. Hardware and project documentation are CERN-OHL-S. Both allow commercial use when their obligations are met. For closed-source use, contact WeChat Yet_Latitude for a separate commercial license. Sharing, reposting, recommending, and promoting this project is welcome.
YHorizon-JM 的软件按 GPLv3、硬件与项目文档按 CERN-OHL-S 开源，遵守开源义务即可商用。希望闭源使用的，请联系微信 Yet_Latitude。欢迎转发、分享和宣传。
-->

Open-source joint motor. Project site: [https://yhorizon.tech/jm/](https://yhorizon.tech/jm/). The firmware, host, communication SDK, and tools in this repository target the open-source version of the production unit: Classic CAN, GD32F303 driver board. Firmware 1.6, host 0.3.0, communication SDK 0.1.0.

> **This project is published under open-source licenses.**
> Mechanical, electronics, firmware, and SDK sources are all published.
> GPLv3 and CERN-OHL-S allow commercial use when their obligations are met. For closed-source use, take a separate [commercial license](#commercial-cooperation).
> It does not guarantee reliability or safety, and it is not a product certification or quality commitment.
> Anyone who uses this project assumes the risk of personal injury, equipment damage, and other loss. See [SECURITY.en.md](SECURITY.en.md).

<p align="center">
  <img src="images/Standing.jpg" alt="Open-source version of the production unit" width="48%"/>
  <img src="images/Flange.jpg" alt="Output flange" width="48%"/>
</p>
<p align="center">
  <img src="images/Linkage.jpg" alt="Mounted on a connecting link" width="70%"/>
</p>
<p align="center">
  Open-source version of the production unit · output flange · mounted on a connecting link
</p>

<p align="center">
  <img src="images/YH-JM1-section-A-A.png" alt="YH-JM1 axial section A—A" width="70%"/>
</p>
<p align="center">
  Axial section A—A
</p>

## Current actuator

Integrated joint with a planetary gearbox. Mechanics and electronics of the open-source version of the production unit:

| Item | Status |
| --- | --- |
| Envelope | Diameter 57 mm, thickness 52 mm |
| Mass | About 300 g |
| Motor | Frameless WK4310H, 14 pole pairs |
| Gear | Planetary, ratio 8 |
| Torque | 3 Nm continuous, 6 Nm peak |
| Supply | Up to 48 V |
| Encoder | Dual MT6701 vernier (30/31 teeth); multi-turn motor shaft is recovered, then divided by the gear ratio for the output shaft |
| MCU | GD32F303CCT6, 112 MHz |
| Power stage | FD6288 three-phase driver |
| Bus | Classic CAN 2.0, 1 Mbps, Motor Protocol V1.0 |
| Control | Firmware 1.6. Current inner loop by default; voltage inner loop optional. Motion / velocity / position modes, plus group sync for 1–16 axes |

After power-on calibration or loading Flash parameters, the drive enters motion control. The current PI uses the WK4310H phase resistance and inductance in this repo. A purchased motor may not match those parameters. If the current loop will not run, set `CFG_INNER_LOOP` in `firmware/App/config.h` to `CFG_INNER_LOOP_VOLTAGE` and rebuild to switch back to the voltage inner loop. Protocol: [`sdk/protocol/Motor CAN Protocol V1.0.en.md`](sdk/protocol/Motor%20CAN%20Protocol%20V1.0.en.md).

Test video: [Stage-1 joint motor complete | D57 H52, 8:1, 3 Nm continuous](https://www.bilibili.com/video/BV1CEtB6QELB/)

Group sync is already implemented on Classic CAN in firmware 1.6. See [Multi-axis position sync](docs/multi-axis.md). A later revision still plans to move the MCU to **GD32C113** for **CAN FD**. Mechanical interfaces and the gear train should stay compatible where possible; electronics and protocol will change with CAN FD.

## Build one

Start with the two BOMs for sourcing parts and fabricating the board. Do not order from other files first:

- Mechanical parts and purchased hardware: [`mechanical/bom/组装购买清单.xlsx`](mechanical/bom/组装购买清单.xlsx)
- Driver electronics: [`hardware/bom/BOM_YHorizon-JM.xlsx`](hardware/bom/BOM_YHorizon-JM.xlsx)

PCB source (JLCPCB / EasyEDA Pro): [`hardware/easyeda/ProPrj_YHorizon-JM.epro2`](hardware/easyeda/ProPrj_YHorizon-JM.epro2). Gerber files: [`hardware/fabrication/Gerber_YHorizon-JM.zip`](hardware/fabrication/Gerber_YHorizon-JM.zip). Schematic PDF: [`hardware/schematic/SCH_YHorizon-JM.pdf`](hardware/schematic/SCH_YHorizon-JM.pdf). Discussion: QQ group **1065885316**.

Assembly STEP: [`mechanical/asm/YHorizon-JM-asm.STEP`](mechanical/asm/YHorizon-JM-asm.STEP). CNC parts are in `mechanical/cnc/`. **CNC parts are not guaranteed to match ours; try fits and adjust hole positions to your own machining.** Check the threaded-hole notes before ordering:

<p align="center">
  <img src="images/CNC下单注意事项螺纹孔1.png" alt="CNC order notes: threaded holes 1" width="48%"/>
  <img src="images/CNC下单注意事项螺纹孔2.png" alt="CNC order notes: threaded holes 2" width="48%"/>
</p>
<p align="center">
  <img src="images/CNC下单注意事项螺纹孔3.png" alt="CNC order notes: threaded holes 3" width="48%"/>
  <img src="images/CNC下单注意事项螺纹孔4.png" alt="CNC order notes: threaded holes 4" width="48%"/>
</p>
<p align="center">
  CNC order notes (threaded holes)
</p>

The YHorizon kit we put together is also on Xianyu, 50 sets (code **CZ007**): [Xianyu listing](https://m.tb.cn/h.jNge4x4?tk=GeXY6fdh1uc).

<p align="center">
  <img src="images/xianyu.jpeg" alt="Xianyu YHorizon kit listing" width="35%"/>
</p>
<p align="center">
  Xianyu YHorizon kit listing
</p>

You also need tools that are not on the BOM, including:

- An arbor press (bearings, planet carrier, and similar fits)
- Loctite threadlocker / retaining compound
- Hex keys (Allen wrenches)
- Programmer cable: 0.8 mm insulation-displacement (pierce) terminal wire

Videos:

- Test: [Stage-1 joint motor complete | D57 H52, 8:1, 3 Nm continuous](https://www.bilibili.com/video/BV1CEtB6QELB/)
- Full assembly: [Douyin assembly video](https://v.douyin.com/Lvc5RXRxO6M/)

Use of the project must meet the GPLv3 and CERN-OHL-S obligations. To keep derivative work closed, use [Commercial cooperation](#commercial-cooperation). Read [SECURITY.en.md](SECURITY.en.md) before assembly or power-up.

## Layout

| Path | Contents |
| --- | --- |
| `mechanical/asm/` | Assembly STEP |
| `mechanical/cnc/` | CNC models |
| `mechanical/gears/` | Gears |
| `mechanical/bom/` | Mechanical BOM |
| `hardware/easyeda/` | EasyEDA Pro / JLCPCB source project |
| `hardware/schematic/` | Schematics |
| `hardware/fabrication/` | Board fabrication files |
| `hardware/bom/` | Electronics BOM |
| `firmware/` | GD32F303 firmware 1.6. Board parameters live in `firmware/Boards/YH_JM/` |
| `host-rs/` | Rust host 0.3.0 |
| `sdk/` | Communication SDK 0.1.0. `sdk/python/joint_gui.py` is still the RS05 / DM4310 debug UI |
| `sdk/protocol/` | CAN protocol |
| `docs/` | Commissioning, current loop, group sync, Ubuntu notes, and protocol-compatibility notes not yet in the firmware |
| `tools/` | Setup, build, and flash scripts |
| `images/` | Photos of the open-source version of the production unit, axial section, CNC order notes, and kit screenshot |

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

`setup-env.sh` installs missing packages with apt and creates `sdk/python/.venv` for the RS05 / DM4310 debug UI. The debug probe is CMSIS-DAP over SWD. Distro OpenOCD usually has no `gd32f30x` driver; the script falls back to `tools/openocd-gd32f303-stm32f1x.cfg` (256 KB, no `mass_erase`). The factory self-test firmware uses preset `gd32f303-factory`.

Motor parameters are in `firmware/Boards/YH_JM/board_config.h`. To switch back to the voltage inner loop, change `CFG_INNER_LOOP` in `firmware/App/config.h`.

## Host

The host is [`host-rs`](host-rs), written in Rust and egui, and it matches firmware 1.6. With no arguments it opens the GUI. The top tabs are Motor, Flash, Commissioning, and Group sync. On the flash page choose YH_JM and `firmware/build/gd32f303/YHorizon-JM.bin`. Behavior is described in [host-rs/README.md](host-rs/README.md). Ubuntu dependencies and SocketCAN are in [docs/ubuntu.md](docs/ubuntu.md). Commissioning steps are in [docs/commissioning.md](docs/commissioning.md).

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

`joint_gui.py` is a separate window for RS05 / DM4310 frames. It does not speak this repository's Motor Protocol V1.0. From `sdk/python`, run `python joint_gui.py` (dependencies are in `requirements.txt`). Codec test: `python -m unittest discover -s sdk/python -p test_joint_protocols.py -v`.

The RS05 / DM4310 notes in `docs/` are not implemented by this firmware. The commissioning, current-loop, and group-sync notes match the current firmware and are written in Chinese.

## Commercial cooperation

The open-source licenses allow commercial use if copyleft derivatives stay public. The open-source version is designed for machining. It works, but optimal performance and long-term stability are not guaranteed. Contact us for the production version.

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
