# Changelog

[中文](CHANGELOG.md) | [English](CHANGELOG.en.md)

## [Unreleased]

- Firmware updated to 1.6: current inner loop by default, commissioning trials, and Classic CAN group sync for 1–16 axes
- Replaced the Python `servo_gui` / `servo_host` tools with the Rust host `host-rs` 0.3.0 and communication SDK 0.1.0
- Updated dimensions and mass (58 mm diameter, 58 mm length, 360 g). Kits and production units ship for 24 V operation. Output torque measured 5 N·m on a 48 V bench, with an estimated peak of about 10 N·m. A 64 V revision requires different power MOSFETs and board-level voltage validation.
- Illustrated current-loop readouts and documented the theoretical power-up vernier span of 3.875 output turns in total.
- Listed the open-source production and Quanzhou CNC kit models separately. Their gear sets are incompatible. The [mechanical BOM](mechanical/bom/README.md) now includes standard hardware specifications and selected purchase links.

- Added [repository guides](guide/README.md) and an [illustrated host manual](host-rs/README.md). The project website and tutorials are under construction.
- Published EasyEDA Pro source and assembly STEP
- Relicensed as GPLv3 for software and CERN-OHL-S for hardware and project documentation, with a separate commercial-license template. The open-source licenses allow commercial use when their obligations are met
