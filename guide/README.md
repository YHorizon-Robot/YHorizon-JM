# 使用与调试指南

[返回项目说明](../README.md)

这里是随仓库公开的工程使用说明，对应固件 1.6 和 Rust 上位机 0.3.0。

[官网装配与调试教学](https://yhorizon.tech/jm/learn/)提供按硬件版本选择起点的准备清单、实物装配图解、上位机工作流，以及机械设计与 FOC 的交互演示和 Agent 学习提示词。先完成机械装配与首次运行，再按需要学习原理部分。

| 文档 | 内容 |
| --- | --- |
| [电流环](current-loop.md) | 默认电流内环的参数、采样和上机核对 |
| [校准与辨识](commissioning.md) | 编码器、R/L、PID、齿槽 |
| [多轴同步](multi-axis.md) | 1～16 轴位置同步的操作和帧格式 |
| [Ubuntu](ubuntu.md) | SocketCAN、上位机和 SDK |

上位机每个页面的图在 [host-rs/README.md](../host-rs/README.md)。

机械制造以[机械 BOM](../mechanical/bom/README.md)和所选版本的加工文件为准；通信字段以[协议定义](../sdk/protocol/Motor%20CAN%20Protocol%20V1.0.md)及[协议编解码源码](../sdk/src/yfoc-protocol/src/lib.rs)为准。官网的交互模型用于学习参数关系，实机参数应通过设备回读、辨识和台架测试确认。
