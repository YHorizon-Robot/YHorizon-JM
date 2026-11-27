# Changelog

[中文](CHANGELOG.md) | [English](CHANGELOG.en.md)

## [Unreleased]

- Firmware updated to 1.6: current inner loop by default, commissioning trials, and Classic CAN group sync for 1–16 axes
- Replaced the Python `servo_gui` / `servo_host` tools with the Rust host `host-rs` 0.3.0 and communication SDK 0.1.0
- Updated dimensions and mass (58 mm diameter, 58 mm length, 360 g). Kits and production units ship for 24 V operation. The 32 V load test ran at 5 N·m for over five minutes; repeatability error was measured at 5 arcminutes. The 48 V bucket-load result of about 6–8 N·m and projected peak above 10 N·m are estimates. A 64 V revision requires different power MOSFETs and board-level voltage validation.
- Illustrated current-loop readouts and documented the theoretical power-up vernier span of 3.875 output turns in total.
- Listed the open-source production and Quanzhou CNC kit models separately. Their gear sets are incompatible. The [mechanical BOM](mechanical/bom/README.md) now includes standard hardware specifications and selected purchase links.

- Updated website, assembly teaching, performance and purchase/order links; documented independent accessory quantities, fulfillment progress and refunds.
- Published EasyEDA Pro source and assembly STEP
- Relicensed as GPLv3 for software and CERN-OHL-S for hardware and project documentation, with a separate commercial-license template. The open-source licenses allow commercial use when their obligations are met
