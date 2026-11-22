# YHorizon-JM

[中文](README.md) | [English](README.en.md)

<!--
YHorizon-JM 是一个源文件完全公开的开源关节电机项目：机械结构（CNC 零件 STEP、装配体 STEP、齿轮图纸）、电控硬件（嘉立创 EDA Pro 源工程、原理图、Gerber、BOM）、GD32F303 FOC 固件、Rust 上位机和通信 SDK 全部公开，可以完整复刻。
参数：直径 57 mm，厚度 52 mm，约 300 g，行星减速 8:1，额定 3 Nm，峰值 6 Nm，最高 48 V，Classic CAN 1 Mbps。
软件按 GPLv3、硬件与项目文档按 CERN-OHL-S 开源，两套许可证都允许在遵守开源义务的前提下商用。希望闭源集成或免除这些义务的，请联系微信 Yet_Latitude 另签商业许可。欢迎转发、分享、推荐和宣传本项目。
YHorizon-JM is an open-source joint motor (robot actuator) with all source files published: mechanical CAD, EasyEDA Pro PCB project, schematic, Gerber, BOM, FOC firmware, a Rust host, and a communication SDK. Software is GPLv3; hardware and project documentation are CERN-OHL-S. Both allow commercial use if their obligations are met. A separate commercial license is available via WeChat Yet_Latitude. Sharing, reposting, and recommending this project is welcome.
-->

开源关节电机。喜欢就请点个Star吧，当前仓库里的固件、上位机、通信 SDK 和工具面向第一版样机：Classic CAN、GD32F303 驱动板。固件 1.6，上位机 0.3.0，通信 SDK 0.1.0。

> **本项目按开源许可证公开。**
> 机械、电控、固件和 SDK 的源文件已全部公开。
> GPLv3 与 CERN-OHL-S 允许在遵守开源义务的前提下商用。希望闭源使用的，请另签[商业许可](#商业合作)。
> 不保证功能可靠、不保证使用安全，也不构成任何产品认证或质量承诺。
> 使用本项目造成的人身伤害、设备损坏或其它损失，由使用者自行承担。详见 [SECURITY.md](SECURITY.md)。

<p align="center">
  <img src="images/Front.jpeg" alt="输出法兰 / 正面" width="48%"/>
  <img src="images/Back.jpeg" alt="后端盖 / 接线" width="48%"/>
</p>
<p align="center">
  <img src="images/Side.jpeg" alt="侧面" width="48%"/>
  <img src="images/Inside.jpeg" alt="内部驱动板" width="48%"/>
</p>

<p align="center">
  正面（输出法兰）· 背面（电源与指示灯）· 侧面 · 内部驱动板
</p>

<p align="center">
  <img src="images/CAD.jpeg" alt="机械 CAD" width="70%"/>
</p>
<p align="center">
  机械结构（CAD）
</p>

## 当前电机

一体化关节，行星减速。第一版机械与电控：

| 项目 | 现状 |
| --- | --- |
| 外形 | 直径 57 mm，厚度 52 mm |
| 重量 | 约 300 g |
| 电机 | 无框电机 WK4310H，14 对极 |
| 减速 | 行星减速机，减速比 8 |
| 扭矩 | 额定 3 Nm，最大 6 Nm |
| 供电 | 最高 48 V |
| 编码器 | 双 MT6701 游标（30/31 齿），合成电机轴多圈后再除以减速比得到输出轴 |
| MCU | GD32F303CCT6，112 MHz |
| 功率级 | FD6288 三相驱动 |
| 总线 | Classic CAN 2.0，1 Mbps，Motor Protocol V1.0 |
| 控制 | 固件 1.6。默认电流内环；可改电压内环。运控 / 速度 / 位置三模式，并支持 1～16 轴组同步 |

JM1 上电读取本机保存的校准和整定结果。没有经过整定的电流 PI 时，使用 WK4310H 板级默认值。首次装配的单元用 `gd32f303` 镜像和 Rust 上位机的「校准与辨识」页完成设置。协议说明见 [`sdk/protocol/Motor CAN Protocol V1.0.md`](sdk/protocol/Motor%20CAN%20Protocol%20V1.0.md)（[English](sdk/protocol/Motor%20CAN%20Protocol%20V1.0.en.md)）。

测试视频：[自研关节电机阶段一完成｜D57 H52，8 倍减速，3Nm 稳定输出](https://www.bilibili.com/video/BV1CEtB6QELB/)

多轴同步已经在固件 1.6 的 Classic CAN 上实现，见[多轴同步位置控制](docs/multi-axis.md)。后续仍计划把主控换成 **GD32C113**，以支持 **CAN FD**。机械接口与减速方案会尽量保持兼容，电控和协议会随 CAN FD 升级。

## 复刻

先对照两份清单备料、打板，不要直接下单目录里的其它文件：

- 机械零件与外购件：[`mechanical/bom/组装购买清单.xlsx`](mechanical/bom/组装购买清单.xlsx)
- 驱动器元器件：[`hardware/bom/BOM_YHorizon-JM.xlsx`](hardware/bom/BOM_YHorizon-JM.xlsx)

PCB 源工程（嘉立创 EDA Pro / EasyEDA Pro）：[`hardware/easyeda/ProPrj_YHorizon-JM.epro2`](hardware/easyeda/ProPrj_YHorizon-JM.epro2)。打板也可用 [`hardware/fabrication/Gerber_YHorizon-JM.zip`](hardware/fabrication/Gerber_YHorizon-JM.zip)，原理图 PDF 见 [`hardware/schematic/SCH_YHorizon-JM.pdf`](hardware/schematic/SCH_YHorizon-JM.pdf)。交流可加 QQ 群 **1065885316**。

装配体 STEP：[`mechanical/asm/YHorizon-JM-asm.STEP`](mechanical/asm/YHorizon-JM-asm.STEP)。CNC 零件在 `mechanical/cnc/`。**CNC 加工不保证和我们完全一致，建议多试、按自己的加工结果修改孔位。** 下单前请对照螺纹孔注意事项：

<p align="center">
  <img src="images/CNC下单注意事项螺纹孔1.png" alt="CNC 下单注意事项：螺纹孔 1" width="48%"/>
  <img src="images/CNC下单注意事项螺纹孔2.png" alt="CNC 下单注意事项：螺纹孔 2" width="48%"/>
</p>
<p align="center">
  <img src="images/CNC下单注意事项螺纹孔3.png" alt="CNC 下单注意事项：螺纹孔 3" width="48%"/>
  <img src="images/CNC下单注意事项螺纹孔4.png" alt="CNC 下单注意事项：螺纹孔 4" width="48%"/>
</p>
<p align="center">
  CNC 下单注意事项（螺纹孔）
</p>

也可以在闲鱼上买我们整理的 YHorizon 套件，一共 50 套（口令 **CZ007**）：[闲鱼链接](https://m.tb.cn/h.jNge4x4?tk=GeXY6fdh1uc)。

<p align="center">
  <img src="images/xianyu.jpeg" alt="闲鱼 YHorizon 套件页面" width="35%"/>
</p>
<p align="center">
  闲鱼 YHorizon 套件页面
</p>

清单之外需要自己准备的工具（不限于）：

- 压力机
- 乐泰（Loctite）螺纹胶 / 固持胶
- 内六角扳手、小十字螺丝刀
- 烧录器线：0.8 mm 刺破式端子线

装配过程请看下面的视频：

- 测试：[自研关节电机阶段一完成｜D57 H52，8 倍减速，3Nm 稳定输出](https://www.bilibili.com/video/BV1CEtB6QELB/)
- 总装：[抖音装配视频](https://v.douyin.com/Lvc5RXRxO6M/)

使用时须遵守 GPLv3 与 CERN-OHL-S 的公开义务。不想公开衍生作品时，请走[商业合作](#商业合作)。装配和上电前请阅读 [SECURITY.md](SECURITY.md)。

## 仓库结构

| 目录 | 内容 |
| --- | --- |
| `mechanical/asm/` | 装配体 STEP |
| `mechanical/cnc/` | CNC 模型 |
| `mechanical/gears/` | 齿轮 |
| `mechanical/bom/` | 零件 BOM |
| `hardware/easyeda/` | 嘉立创 EDA Pro 源工程 |
| `hardware/schematic/` | 原理图 |
| `hardware/fabrication/` | 制板文件 |
| `hardware/bom/` | 元器件 BOM |
| `firmware/` | GD32F303 固件 1.6。板级参数在 `firmware/Boards/YH_JM/` |
| `host-rs/` | Rust 上位机 0.3.0 |
| `sdk/` | 通信 SDK 0.1.0。`sdk/python/joint_gui.py` 仍是 RS05 / CyberGear / DM4310 联调界面 |
| `sdk/protocol/` | CAN 协议 |
| `docs/` | 校准、电流环、多轴同步、Ubuntu 说明，以及尚未做进固件的协议兼容笔记 |
| `tools/` | 环境、编译、烧录脚本 |
| `images/` | 样机照片、CAD、CNC 下单注意事项与套件截图 |

## 固件

需要 `arm-none-eabi-gcc` 与 CMake + Ninja。产物：`firmware/build/gd32f303/YHorizon-JM.elf`。CLion 打开 `firmware/`，选择 preset `gd32f303`。

Windows：

```powershell
.\tools\setup-env.ps1
.\tools\build.ps1
.\tools\flash.ps1
```

Ubuntu：

```bash
./tools/setup-env.sh
./tools/build.sh
./tools/flash.sh
./tools/run-gui.sh
```

`setup-env.sh` 缺什么就用 apt 装什么，并为 RS05 / CyberGear / DM4310 联调界面创建 `sdk/python/.venv`。烧录探针为 CMSIS-DAP + SWD。发行版 OpenOCD 一般没有 `gd32f30x` 驱动，脚本会改用 `tools/openocd-gd32f303-stm32f1x.cfg`（按 256 KB，且不做 `mass_erase`）。工厂自检固件用 preset `gd32f303-factory`。

电机参数在 `firmware/Boards/YH_JM/board_config.h`。切回电压内环仍改 `firmware/App/config.h` 的 `CFG_INNER_LOOP`。

## 上位机

上位机在 [`host-rs`](host-rs)，用 Rust 和 egui 写成，对应固件 1.6。没有参数时打开图形界面，顶部是「电机」「烧录」「校准与辨识」「多轴同步」。烧录页选择 YH_JM，镜像用 `firmware/build/gd32f303/YHorizon-JM.bin`。操作见 [host-rs/README.md](host-rs/README.md)，Ubuntu 依赖和 SocketCAN 见 [docs/ubuntu.md](docs/ubuntu.md)。校准步骤见 [docs/commissioning.md](docs/commissioning.md)。

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

Windows 打开 PCAN 前要安装 [PCAN-Basic](https://www.peak-system.com/PCAN-Basic.239.0.html)。协议说明见 [`sdk/protocol/`](sdk/protocol/)。

## SDK

[关节电机通信 SDK](sdk/README.md) 给自己的控制程序用。Python 和 C++17 加载同一份动态库，Rust 直接依赖 `sdk/src/yfoc-motor-sdk`。在仓库根目录构建，动态库生成在 `sdk/lib`：

```powershell
python sdk/build_native.py
```

```python
from yfoc_motor import Bus

with Bus("socketcan", "can0") as bus:
    sample = bus.feedback(1)
    print(sample.position, sample.velocity)
```

Windows 用 `Bus("pcan", "PCAN_USBBUS1")`。查询、模式、关桥和 `run_group` 的说明都在 [sdk/README.md](sdk/README.md)。

`joint_gui.py` 是另一套界面，只用来对照 RS05 / CyberGear / DM4310 的帧，不控制本仓库固件的 Motor Protocol V1.0。在 `sdk/python` 下执行 `python joint_gui.py`（依赖 `requirements.txt`）。编解码单测：`python -m unittest discover -s sdk/python -p test_joint_protocols.py -v`。

### DM4310 MIT 台架镜像

用 `.\tools\build.ps1 -Preset gd32f303-dm4310`（Linux 用 `./tools/build.sh --preset gd32f303-dm4310`）生成独立镜像。先在原生 V1 上完成本机校准、标零并设置节点 1–15，停机后按页刷写对应 ELF。换回同版 V1 可维护原有校准参数。此镜像采用经典 CAN 1 Mbps，收节点号，反馈 ID 为 `0x10 + 节点号`。例如节点 1 用 `python sdk/python/joint_gui.py --protocol DM4310 --node 1 --host 0x11`。

首版支持 MIT 五字段、使能 FC、失能 FD、清故障 FB，以及定向只读寄存器 7/8/9/10/21/22/23。可先发送标准帧 `7FF#0100331500000000` 读取节点 1 的 PMAX，预期 `011#0100331500004841`。运动前检查机械行程，在 GUI 选择“自由”（Kp、Kd、前馈为零），以 100 Hz 连续发送，再点使能。收到使能或最后一条运动指令后 100 ms 超时关桥，需要重新使能。兼容层忽略寄存器写入；收到 FE 时关桥，不保存零点、不回成功应答。

P/V/T 线量程分别为 ±12.5 rad、±30 rad/s、±10 Nm。位置为相对本机机械零点的输出轴绝对目标，反馈超出编码范围时饱和。台架镜像把含齿槽补偿的 Iq 目标限制在 ±1 A。反馈力矩为按实测 Iq 换算的输出轴估计值，D6 为板温（不可用时填 0），D7 填 0。原厂主控互通及实物关桥延迟待台架验证。

在本机测试通信状态机和报文字节可运行 `cmake -S firmware/tests -B firmware/build/host-tests -G Ninja`、`cmake --build firmware/build/host-tests`、`ctest --test-dir firmware/build/host-tests --output-on-failure`。

### 首次校准和后续换协议

1. **新装或参数区为空的 JM1**：刷 `gd32f303`，它使用 JM1 自有 CAN 协议 Motor Protocol V1.0。连接 Rust 上位机，读取节点，固定空载电机并确认可用行程。依次做编码器校准、机械标零、相电阻及 d/q 轴电感辨识、自动 PID 整定。需要低速平顺性时再做双向齿槽扫描。每项通过后保存并回读，再进行下一项。
2. **已有本机有效记录**：可以继续用 JM1 自有协议，也可以停机、停发后用仓库脚本按页刷目标兼容镜像。两者加载同一组逐机参数。整片擦除会删除这些记录；更换电机、相线或编码器后，应重新确认电角度和方向并更新相关整定。重新保存 R/L 会撤销原自动整定标记和齿槽表，需重新整定；重新保存 PID 后需重新采集齿槽表。
3. **核对运行效果**：Rust 上位机可读取并导出相电阻、Ld/Lq、各环增益和 256 点齿槽表。兼容镜像上电时可通过 RTT 看 `JM1 saved profile` 的代号与标志（十六进制位掩码：01 R/L、02 电流 PI、04 外环 PID、08 编码器、10 游标、20 齿槽）。调试输出 `tmax` 是按有效相电阻、母线和当前镜像电流上限估计的电机轴堵转力矩。

低速速度、位置和有刚度或阻尼的阻抗指令会使用已启用的齿槽表。直接指定电流、纯力矩以及零增益自由模式保持主机要求的电流语义。阻抗报文中的 Kp/Kd 仍由主机给定，电流内环使用本机整定值。切换协议无需重新生成补偿表。

### 切换到其他 CAN 台架镜像

每个镜像使用一种型号配置，经典 CAN 1 Mbps。以下报文示例均为节点 1。首次使用先在 V1 下设置节点、校准并保存机械零位。速度或位置控制还需在 V1 下整定速度 PI 和位置环。换协议时先停止旧主机发送并关桥，在同一台电机上按页刷目标 ELF，再连接对应的主机。RS05 与小米、DM 与 AK、两款 DJI 均有可相互匹配的控制帧，同一条总线上的所有发送端须使用相符的量程。原厂上位机对未实现参数写入的依赖需要逐版本实测。

| 构建 preset 后缀 | 关桥时的连通检查 | 解锁、运动及停机 |
| --- | --- | --- |
| `rs05`、`cybergear` | EXT `0000FD01#0000000000000000` → EXT `000001FE`，8 字节芯片标识 | 主站固定 `FD`。type 3 使能，1 阻抗，2 回传，4 停止或清错。6 在已标定、失能且速度 ≤0.05 rad/s 时设置 RAM 零，复位恢复持久基准。17 可读 7005/7019/701B/701C（十六进制）；18 写入及保存命令关闭。节点 1–63 |
| `ak60-6` | STD `001#FFFFFFFFFFFFFFFD`，失能并在同 ID 回报，D0 为节点 | MIT 的 FC 使能、FD 失能；位置/速度/力矩量程 ±12.5 rad、±45 rad/s、±15 Nm。D6 为板温 +40，D7 为故障码。节点 1–63，持久零位用 V1。[CubeMars AK 手册 §5.3](https://www.cubemars.com/images/file/20240523/1716429938564781.pdf) |
| `rmd-x` | STD `141#9C00000000000000` → `241`。92 读多圈角，D4–7 为有符号 32 位 | V3.91。A1 零电流解锁，后续 A1 电流、A2 速度、A4 带限速的绝对位置。80 关桥。台架版将 81 也处理为关桥，需重新解锁，依赖主动制动的主机不适用。节点 1–32。[MyActuator V3.91](https://teika-machine.co.jp/wp-content/uploads/2024/10/RMD-X-Motor-Motion-Protocol-V3.91.pdf) |
| `lk-mf` | 同一 9C 请求，回复也是 STD `141`。92 的 D1–7 为有符号 56 位 | MF V2.35。88 使能，A1/A2/A4 控制，80/81 关桥。将 JM1 输出轴视为 MF 轴，9C 报 14 位单圈角；A1 的 ±2048 对应 ±16.5 A，9C/A2/A4 回复依照各自表格用 ±33 A。节点 1–32。[MF V2.35 §4–5](https://www.scribd.com/document/789874778/MS-MF-RMD-motor-CAN-protocol-V2-35-3-1) |
| `hightorque` | EXT `00008001#1701` → EXT `00000100#2701…`；也接受该查询补零到 8 字节 | 经典 CAN，主站 0，节点 1–63。4 字节 `05 13 00 00` 解锁，后续同命令的小端 int16 为纯力矩。3 字节 `01 00 00` 停机。采用 STM32 示例定标，位置 0.0001 圈、速度 0.00025 圈/s、力矩 0.01 Nm。厂商手册还列有其他速度单位和型号拟合系数，原控实测前仅按此示例联调。混控、广播、FD 和校准命令关闭。[高擎经典 CAN 示例 §3.2](https://hightorque.cn/en/wp-content/uploads/2024/05/High-TorqueDebugging-Manual.pdf) |
| `c620`、`c610` | 被动监听 STD `201`，目标周期 1 ms。节点 1–4 收 `200`，5–8 收 `1FF` | 本槽大端 int16 零电流解锁，新帧非零才出力。C620 为 ±16384 ↔ ±20 A，C610 为 ±10000 ↔ ±10 A；两款均受 JM1 台架限流。反馈角度/转速取实际转子侧，C620 D6 报板温，C610 D6–7 为零。用本槽零电流停止，停发超时后需再发零。[DJI C610 CAN 指南](https://rm-static.djicdn.com/tem/17348/RoboMaster%20C610%20Brushless%20DC%20Motor%20Speed%20Controller%20User%20Guide.pdf) |

RS05 沿用仓库 SDK 的两套量程，控制为 ±12.57 rad、±33 rad/s、±14 Nm，反馈为 ±4π rad、±50 rad/s、±5.5 Nm。CyberGear 收发均为 ±4π rad、±30 rad/s、±12 Nm。两种镜像均可用 Python GUI 的同名选项联调。RMD-X/LK 的 A2、A4 需要有效外环增益。各型号使用校准结果加载时确定的输出轴方向，温度字段取 JM1 板温，输出力矩按实测 Iq 估算。

**构建、刷写与回退**（仓库根目录）：

```powershell
.\tools\build.ps1 -Preset gd32f303-cybergear
# 确认已停机且旧主机停发后，选择这一型号
.\tools\flash.ps1 -Preset gd32f303-cybergear
python sdk/python/joint_gui.py --protocol CyberGear --node 1 --host 0xFD
```

各镜像保留 V1 的 A/B 和 legacy 校准页。只换镜像时复用本机校准；更换电机、编码器或相线后回 V1 重校。所有兼容镜像将含补偿的 Iq **目标**限制在 ±1 A，100 ms 未续期会在主循环关桥；查询不续期，掉线或 CAN 接收丢失后需重新握手。原厂主控互通、实际峰值电流、最坏关桥延迟和 DJI 多机反馈负载仍需台架测量。

运行 `python tools/package-compat.py` 可重建九个兼容镜像与同版 V1，生成 `firmware/build/JM1-CAN-bench.zip`。包内 manifest 记录各 ELF 的 SHA-256 与 Flash 加载末址，脚本校验所有有效加载段均在 `0x08000000..0x0803E7FF` 内。回退时停机、停发，将包内 `JM1-v1.elf` 解压后用 `.\tools\flash.ps1 -ElfFile <文件路径>` 按页刷回，并在 V1 下复读原机参数。

## 商业合作

开源许可证允许商用，条件是继续公开受 copyleft 约束的衍生软件和硬件设计。仓库里的结构偏学习、易加工和低成本，不适合直接当作量产产品。

希望在不公开修改的情况下集成、制造或销售，请联系微信 **Yet_Latitude** 另签商业许可。模板见 [`COMMERCIAL-LICENSE.md`](COMMERCIAL-LICENSE.md)。未书面签署的模板不会生效，也不改变开源许可证已经给出的权利。

## 许可证

双许可，详见 [`LICENSE`](LICENSE)。开源许可允许在遵守其义务的前提下商用。商业许可是另外一条路径，只在另行书面签署后生效。

| 内容 | 许可证 | 要点 |
| --- | --- | --- |
| `mechanical/`、`hardware/`、`images/` 及配套项目文档 | [CERN-OHL-S-2.0](LICENSES/CERN-OHL-S-2.0.txt) | 强互惠开源硬件许可。分发产品或修改版设计时，须按 CERN-OHL-S 提供相应设计资料 |
| `firmware/`、`host-rs/`、`sdk/`、`tools/` | [GPLv3](LICENSES/GPL-3.0.txt) | 允许商用；分发衍生软件时须以 GPLv3 提供完整对应源码 |
| 另签 | [商业许可模板](COMMERCIAL-LICENSE.md) | 用于不按上述开源义务闭源使用的情形。联系微信 Yet_Latitude；签署前不生效 |


## 安全

完整免责声明见 [SECURITY.md](SECURITY.md)。关节电机能输出较大力矩；未完成限位、过流和急停验证前，不要在人体附近满功率运行。

## 贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md)。
