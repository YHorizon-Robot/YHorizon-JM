# 贡献指南

[中文](CONTRIBUTING.md) | [English](CONTRIBUTING.en.md)

一次改动尽量只动一个模块。较大的接口变化（MCU、减速比、通信协议）请先开 Issue 讨论。

| 内容 | 目录 |
| --- | --- |
| CNC 模型、装配体、齿轮、零件 BOM | `mechanical/` |
| 嘉立创源工程、原理图、制板文件、元器件 BOM | `hardware/` |
| 嵌入式软件 | `firmware/` |
| 上位机 | `host-rs/` |
| 通信 SDK | `sdk/` |
| 标定 / 调参 / 烧录 | `tools/` |

二进制设计文件请同时提供可交换格式（STEP、Gerber、PDF）。不要提交编译产物、密钥或许可证不兼容的第三方文件。

向本仓库提交贡献，即表示你有权作出该贡献，并同意：

- `mechanical/` 与 `hardware/` 中的贡献按 CERN-OHL-S-2.0 授权；
- `firmware/`、`host-rs/`、`sdk/`、`tools/` 中的贡献按 GPLv3 授权；
- 项目文档按 [`LICENSE`](LICENSE) 中对应材料的开源许可证授权；
- 著作权人可以把包含该贡献的项目，在上述开源许可证之外再按商业许可提供。

该商业许可不减少任何人依据开源许可证已经获得的权利。
