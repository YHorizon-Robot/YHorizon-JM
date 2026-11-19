# Contributing

[中文](CONTRIBUTING.md) | [English](CONTRIBUTING.en.md)

Keep a change in one module when you can. Open an Issue first for large interface changes (MCU, gear ratio, protocol).

| Content | Directory |
| --- | --- |
| CNC models, assembly, gears, mechanical BOM | `mechanical/` |
| EasyEDA / JLCPCB source, schematics, fabrication files, electronics BOM | `hardware/` |
| Embedded software | `firmware/` |
| Host SDK | `sdk/` |
| Calibration / tuning / flashing | `tools/` |

Binary design files should include an interchange format (STEP, Gerber, PDF). Do not commit build artifacts, secrets, or third-party files whose licenses are incompatible. Use Doxygen block comments in code.

By submitting a contribution you confirm that you have the right to submit it, and you agree that:

- contributions under `mechanical/` and `hardware/` are licensed under CERN-OHL-S-2.0;
- contributions under `firmware/`, `sdk/`, and `tools/` are licensed under GPLv3;
- project documentation is licensed under the open-source license that [`LICENSE`](LICENSE) applies to the corresponding material;
- the copyright holder may also offer the project, including your contribution, under a commercial license.

That commercial license does not reduce rights anyone already has under the open-source licenses.
