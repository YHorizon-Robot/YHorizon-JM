# YHorizon-JM

[中文](README.md) | [English](README.en.md)

<!--
YHorizon-JM 是一个源文件完全公开的开源关节电机项目：机械结构（CNC 零件 STEP、装配体 STEP、齿轮图纸）、电控硬件（嘉立创 EDA Pro 源工程、原理图、Gerber、BOM）、GD32F303 FOC 固件和 Python 上位机 SDK 全部公开，可以完整复刻。
参数：直径 57 mm，厚度 52 mm，约 300 g，行星减速 8:1，额定 3 Nm，峰值 6 Nm，最高 48 V，Classic CAN 1 Mbps。
软件按 GPLv3、硬件与项目文档按 CERN-OHL-S 开源，两套许可证都允许在遵守开源义务的前提下商用。希望闭源集成或免除这些义务的，请联系微信 Yet_Latitude 另签商业许可。欢迎转发、分享、推荐和宣传本项目。
YHorizon-JM is an open-source joint motor (robot actuator) with all source files published: mechanical CAD, EasyEDA Pro PCB project, schematic, Gerber, BOM, FOC firmware, and Python SDK. Software is GPLv3; hardware and project documentation are CERN-OHL-S. Both allow commercial use if their obligations are met. A separate commercial license is available via WeChat Yet_Latitude. Sharing, reposting, and recommending this project is welcome.
-->

开源关节电机。喜欢就请点个Star吧，当前仓库里的固件、SDK 和工具面向第一版样机：Classic CAN、GD32F303 驱动板。

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
| 控制 | 默认电流内环；可改电压内环。运控 / 速度 / 位置三模式 |

上电校准或加载 Flash 参数后进入运控。电流环 PI 按仓库里的 WK4310H 相电阻、相电感整定。买到的电机参数可能不一样，电流环跑不通时，把 `firmware/App/config.h` 的 `CFG_INNER_LOOP` 改成 `CFG_INNER_LOOP_VOLTAGE`，重新编译即可切回电压内环。协议说明见 [`sdk/protocol/Motor CAN Protocol V1.0.md`](sdk/protocol/Motor%20CAN%20Protocol%20V1.0.md)（[English](sdk/protocol/Motor%20CAN%20Protocol%20V1.0.en.md)）。

测试视频：[自研关节电机阶段一完成｜D57 H52，8 倍减速，3Nm 稳定输出](https://www.bilibili.com/video/BV1CEtB6QELB/)

后续计划把主控换成 **GD32C113**，以支持 **CAN FD** 和多轴同步。机械接口与减速方案会尽量保持兼容，电控和协议会随 CAN FD 升级。

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

可以按开源许可证复刻、制造和销售，但必须遵守 GPLv3 与 CERN-OHL-S 的公开义务。不想公开衍生作品时，请走[商业合作](#商业合作)。装配和上电前请阅读 [SECURITY.md](SECURITY.md)。

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
| `firmware/` | GD32F303 固件 |
| `sdk/python/` | 上位机 Python SDK / GUI |
| `sdk/protocol/` | CAN 协议 |
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

`setup-env.sh` 缺什么就用 apt 装什么，并创建 `sdk/python/.venv`。烧录探针为 CMSIS-DAP + SWD。发行版 OpenOCD 一般没有 `gd32f30x` 驱动，脚本会改用 `tools/openocd-gd32f303-stm32f1x.cfg`（按 256 KB，且不做 `mass_erase`）。

## 上位机

Ubuntu 跑完 `./tools/setup-env.sh` 后可直接 `./tools/run-gui.sh`。也可手动：

```bash
cd sdk/python
python3 -m venv .venv
source .venv/bin/activate   # Windows: .venv\Scripts\Activate.ps1
pip install -r requirements.txt
python servo_gui.py
```

CLI 示例：`python servo_host.py --interface socketcan --channel can0 --id 1 --listen`。协议说明见 `sdk/protocol/`。

## 商业合作

开源许可证允许商用，条件是继续公开受 copyleft 约束的衍生软件和硬件设计。仓库里的结构偏学习、易加工和低成本，不适合直接当作量产产品。

希望在不公开修改的情况下集成、制造或销售，请联系微信 **Yet_Latitude** 另签商业许可。模板见 [`COMMERCIAL-LICENSE.md`](COMMERCIAL-LICENSE.md)。未书面签署的模板不会生效，也不改变开源许可证已经给出的权利。

## 许可证

双许可，详见 [`LICENSE`](LICENSE)。开源许可允许在遵守其义务的前提下商用。商业许可是另外一条路径，只在另行书面签署后生效。

| 内容 | 许可证 | 要点 |
| --- | --- | --- |
| `mechanical/`、`hardware/`、`images/` 及配套项目文档 | [CERN-OHL-S-2.0](LICENSES/CERN-OHL-S-2.0.txt) | 强互惠开源硬件许可。允许制造和销售；分发产品或修改版设计时，须按 CERN-OHL-S 提供相应设计资料 |
| `firmware/`、`sdk/`、`tools/` | [GPLv3](LICENSES/GPL-3.0.txt) | 允许商用；分发衍生软件时须以 GPLv3 提供完整对应源码 |
| 另签 | [商业许可模板](COMMERCIAL-LICENSE.md) | 用于不按上述开源义务闭源使用的情形。联系微信 Yet_Latitude；签署前不生效 |


## 安全

完整免责声明见 [SECURITY.md](SECURITY.md)。关节电机能输出较大力矩；未完成限位、过流和急停验证前，不要在人体附近满功率运行。

## 贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md)。
