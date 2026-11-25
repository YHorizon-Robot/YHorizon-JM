# YHorizon-JM 上位机

## 按已烧录的固件选上位机

同一台 JM1 一次运行一种 CAN 协议，由烧录的固件决定。

- **原生 V1（preset `gd32f303`）**使用 Rust 上位机 `yfoc-host`。图形界面和 CLI 支持运行控制、采集反馈、烧录、校准与辨识、齿槽补偿和多轴同步。[Rust 源码](crates/yfoc-host/src/)
- **DM4310、RS05、CyberGear 兼容镜像**使用独立的 Python 联调 GUI。工具里选与镜像完全一致的协议。[Python GUI 联调](#python-gui-联调)
- **AK60-6、RMD-X、LK-MF、HighTorque、DJI C610 和 C620 兼容镜像**使用 CAN 分析仪或 `python-can` 按型号收发帧。[按报文联调](#按报文联调其余六款)

## 构建和连接原生上位机

在仓库根目录执行。Windows 产物是 `host-rs\target\release\yfoc-host.exe`，双击或直接运行即可打开图形界面。`target` 是本地构建目录，切换分支或更新代码后重新执行构建命令即可更新程序。

```powershell
cargo build --release --locked --manifest-path host-rs/Cargo.toml -p yfoc-host
.\host-rs\target\release\yfoc-host.exe
```

Windows 的 PCAN-USB 需要系统驱动和可加载的 `PCANBasic.dll`，常用通道为 `PCAN_USBBUS1`。可用 `--list-can` 查看检测到的通道。以下示例以台架节点 3 为例，换板时以扫描结果为准。[PCAN-Basic](https://www.peak-system.com/PCAN-Basic.239.0.html)

```powershell
.\host-rs\target\release\yfoc-host.exe --list-can
.\host-rs\target\release\yfoc-host.exe --interface pcan --channel PCAN_USBBUS1 --scan
.\host-rs\target\release\yfoc-host.exe --interface pcan --channel PCAN_USBBUS1 --id 3 --status --current-config --current-state
```

原生和兼容镜像均使用经典 CAN 1 Mbit/s。Linux 的 SocketCAN 波特率由系统配置，例如 `sudo ip link set can0 up type can bitrate 1000000`。Rust 上位机支持 `pcan`、`socketcan` 和 `slcan`。SLCAN 使用 115200 的串口，CAN 波特率在打开时下发。

在「电机」页选择接口和通道、连接并扫描节点。选中设备后先检查状态、告警、位置、内环及电流配置。查看 Iq 曲线时，先打开顶部「电流轮询」，再在示波器选择 Iq 或 Iq_ref。若扫描原生节点无回复，先核对板上镜像和实际 CAN 通道。兼容镜像的连通检查见本文各型号的对应入口。

界面上方是总线总览表，一台电机一行，显示状态、故障、模式、位置、速度、Iq、母线电压、反馈频率和当前活动。行尾的「停止」「清故障」直接作用于该行，点击行其余部分选中节点。下方左侧是选中节点的运行控制（速度、位置、Motion、电流），外环不符时自动先发 `SET_CONTROL_MODE` 并等 ACK；右侧是增益、电流环、管理、信息四页。示波器可以同时勾选多台。顶部还有「烧录」「校准与辨识」「多轴同步」。

总览栏可调三个轮询频率：状态（默认 2 Hz）、电流实测（默认关闭，看电流曲线时打开）、空闲反馈（默认 10 Hz，只对 1.3 固件且未在循环的节点发 `GET_FEEDBACK`）。

「停止」停掉本节点循环；1.3 固件还会发 `DISABLE` 关桥。「全部停止」或 Esc 停掉所有循环，直接电流节点补发 0 A，再向 `0x200` 广播 `DISABLE`；断开连接时同样处理。1.2 及更早的固件没有 `DISABLE`，停止只是不再发送，电机保持最后一帧目标。收到故障状态（包括 1.3 的主动故障上报）时，上位机停止该节点的发送。

1.4 固件：能力 bit6 表示焊了 NTC，状态里的温度有效，过温警告按原协议 bit0 显示为「过温」。能力 bit5 表示双编码器板的多圈未建立警告占用 Byte7 bit4，总览表用黄色显示「多圈未建立」。警告不会让上位机停止发送。

## 电流内环

连接或改变节点 ID 时先查询设备能力和状态。本分支的原生镜像返回三页配置。第 0 页是电流限幅、标幺限幅和带宽，第 1 页是电流 PI 和环频率，第 2 页是转矩常数、减速比和扩展能力。配置查询是只读的。校准与辨识页可测量本机 R/L、整定并保存 PI；有效自动整定记录优先于板级初值，最大电流由固件配置。

上位机收齐三页配置后再请求对应的电流实测。缺失页在 1 秒后重试一次；仍超时则清除不完整配置并恢复刷新按钮，可手动重试。

「电流环」页可以切换电压和电流内环。切换先停止上位机循环；成功后固件清空实时指令并按新内环的默认值复位外环增益，上位机清空相关输入，并重新读取状态和配置。Motion 增益清零。电流内环的速度增益默认值为零，固件也会重新应用有效的已存整定值。切换后在「增益」页回读，确认增益与当前内环匹配再发新目标。内环选择保存在 RAM，复位后使用固件默认值。

直接输入电流时，必须已确认电流内环且完整收到当前设备配置。配置第 2 页 Byte6 bit0 必须为 1。上位机通过 `0x4C0 + ID` 直接发送有符号毫安，允许范围为设备限幅与 20 A 的较小值。旧固件缺少此能力位时，直接电流入口会提示升级。输入是控制方向的电流指令，电角度坐标下的有符号 `Iq_ref` 以电流反馈帧为准。

直接电流先设置 Motion 外环，收到成功 ACK 后发独立 Iq 帧。已配置的 Motion 增益留给下一条 Motion 指令使用。错误或超时会取消后续发送。Motion 前馈（CLI 的 `--ff`）单位为输出轴 Nm，低速且齿槽表有效时会加补偿。直接 Iq（`--current`）跳过补偿。能力、内环和电流配置均按节点分别记录。

「运动 MIT」中的输出轴力矩前馈用 Nm。有效齿槽表在低速电流内环下参与补偿，零增益的 0 Nm 也可能有补偿电流。「电流」中的「直接 Iq」用 A，直接 0 A 会按电流斜坡归零。「发送一次」的目标由原生固件持续保持。要关桥，点「停止」或「全部停止」。退出或切换控制动作时，上位机会撤掉待发实时帧并给正在直接电流控制的节点补发 0 A。

## 反馈

- `t_ref` 是标幺指令，不是实测电流或实测转矩。
- 「电流环」页显示实测 `Id`、实测 `Iq`、指令 `Iq_ref`（A）以及 PI 输出 `Vq`（pu）。电流实测通过独立请求读取，可手动刷新或开启轮询。
- 示波窗口右键可以添加 `Id`、`Iq`、`Iq_ref`、`Vq`；未知电流没有伪造的零值。电流和运动帧分别按到达时间记录，“按帧”统计的是这两类采样点。
- 电流采样失效显示 `CURRENT_SAMPLE_FAULT`，过流显示 `OVER_CURRENT`。上位机检测到节点故障后停止该节点的循环发送。

旧固件不支持能力查询或没有 bit3 时，仍可使用原来的 Motion／Velocity／Position／Gains 功能。内环显示“未知”，反馈只保留 pu，不推断安培值，也不会轮询不支持的电流命令。

## CLI

接口和通道按设备替换。下面把 `yfoc-host` 换成 `target/release/yfoc-host`（Windows 为 `target\release\yfoc-host.exe`）。示例使用 Linux SocketCAN；Windows 可用 `--interface pcan --channel PCAN_USBBUS1`。

```bash
# 只查询，不发送运动指令
yfoc-host --interface socketcan --channel can0 --id 1 --status
yfoc-host --interface socketcan --channel can0 --id 1 --current-config
yfoc-host --interface socketcan --channel can0 --id 1 --current-state

# 切换内环，清除旧指令与增益，然后退出
yfoc-host --interface socketcan --channel can0 --id 1 --inner-loop current

# 明确切到电流内环，按 200 Hz 发送 0.2 A；Ctrl+C 会发送 0 A
yfoc-host --interface socketcan --channel can0 --id 1 --inner-loop current --current 0.2 --rate 200

# 当前分支固件确认在电流内环后，发 0 A 并在收到反馈后关桥退出
yfoc-host --interface socketcan --channel can0 --id 1 --current 0 --once

# 完全被动监听，不发送设备查询
yfoc-host --interface socketcan --channel can0 --id 1 --listen
```

`--current` 不会默默切换内环；需要设备已经处于电流内环，或显式加 `--inner-loop current`。CLI 的直接电流操作在正常退出、Ctrl+C 或异常退出前都会尝试发送 0 A；因此 `--current ... --once` 是收到一次反馈后归零退出。较旧的固件可能只实现部分电流配置页。若扫描能找到设备，但 CLI 读取配置报 PARAMETER_OUT_OF_RANGE，更新同分支的 V1 固件后再试直接 Iq。`--listen` 不主动查询，因此无法获知旧帧中未提供的内环信息。

## 烧录、校准与协议切换

1. 用本分支 `gd32f303` 原生镜像建立本机参数。在「烧录」页选择 YH_JM、SWD 探针和 `firmware/build/gd32f303/YHorizon-JM.bin`。在「校准与辨识」页读取节点，依次完成编码器校准、在机械基准处标零、R/L 辨识、PID 整定和齿槽扫描。每项通过后保存并回读，需要留档时导出参数。也可在仓库根目录用 `.\host-rs\target\release\yfoc-host.exe --id 3 --commission export --commission-output jm1-node3.json` 导出。
2. 已有有效校准的同一台电机可以直接换镜像。先在当前上位机停机并停发，再用对应 preset 构建和按页烧录。初次切换前按目标协议允许的节点范围设置 ID。使用速度或位置的兼容指令前，在 V1 下先确认对应外环增益有效。
3. 烧录后打开匹配的联调工具，先读取反馈、查询节点或执行下文的连通检查，再进入使能和运动。需要改持久零位、节点号或重新校准时，停机后按页刷回同版 `gd32f303`。按页烧录保留校准和整定数据，整片擦除会清除它们。[原生 CAN 协议](../sdk/protocol/Motor%20CAN%20Protocol%20V1.0.md) · [兼容镜像说明及型号定标](../README.md#切换到其他-can-台架镜像)

以下在仓库根目录演示 CyberGear 镜像及回退。请在切换前停止当前上位机的发帧循环。

```powershell
.\tools\build.ps1 -Preset gd32f303
.\tools\build.ps1 -Preset gd32f303-cybergear
.\tools\flash.ps1 -Preset gd32f303-cybergear
# 联调完成，先失能、停发，再刷回原生版
.\tools\flash.ps1 -Preset gd32f303
```

## Python GUI 联调

`sdk/python/joint_gui.py` 是另一套界面，用 PySide6 写成，只讲 RS05、CyberGear、DM4310 三种兼容镜像。原生固件 Motor Protocol V1.0 仍用上面的 Rust 上位机 `yfoc-host`：烧录、校准、参数辨识和多轴同步都不在这个 Python 窗口里。板上如果还是 `gd32f303` 原生镜像，打开 `joint_gui.py` 不会得到本协议的反馈。

窗口里可以做的事：选择协议、连接 CAN、使能、失能、标零、清故障，按位置、速度、阻抗或力矩下发，并看位置、速度、力矩、温度、状态和三条滚动曲线。协议下拉必须和已经烧进板子的镜像一致。编解码在 `sdk/python/joint_protocols.py`。

在仓库根目录执行。首次使用先装依赖。下面以节点 3、PCAN 第一只适配器为例。Linux 把 `--interface pcan --channel PCAN_USBBUS1` 换成 `--interface socketcan --channel can0`，并先 `sudo ip link set can0 up type can bitrate 1000000`。

```powershell
python -m pip install -r sdk/python/requirements.txt

# gd32f303-dm4310，反馈 ID = 0x10 + 节点号，节点范围 1–15
python sdk/python/joint_gui.py --interface pcan --channel PCAN_USBBUS1 --protocol DM4310 --node 3 --host 0x13

# gd32f303-rs05，兼容固件的主站 ID 固定为 0xFD
python sdk/python/joint_gui.py --interface pcan --channel PCAN_USBBUS1 --protocol RS05 --node 3 --host 0xFD

# gd32f303-cybergear，兼容固件的主站 ID 固定为 0xFD
python sdk/python/joint_gui.py --interface pcan --channel PCAN_USBBUS1 --protocol CyberGear --node 3 --host 0xFD
```

`--node` 是电机节点号，`--host` 是主站 ID。DM4310 的主站 ID 填 `0x10 + 节点号`，所以上面节点 3 用 `0x13`。RS05 和 CyberGear 的主站固定 `0xFD`。三条命令不要混用：协议、镜像、主站 ID 必须是同一套。

量程由协议决定，窗口切换协议时会改提示：

| 协议 | 位置 | 速度 | 力矩 |
| --- | --- | --- | --- |
| RS05 控制帧 | ±12.57 rad | ±33 rad/s | ±14 Nm |
| RS05 反馈帧 | ±4π rad | ±50 rad/s | ±5.5 Nm |
| CyberGear | ±4π rad | ±30 rad/s | ±12 Nm |
| DM4310 | ±12.5 rad | ±30 rad/s | ±10 Nm |

Kp 为 0..500，Kd 为 0..5。位置是相对本机机械零点的输出轴角度。

1. 点「连接」。RS05 和 CyberGear 可点「Ping」，检查收帧日志中的芯片标识回复。DM4310 可用分析仪发送 STD `7FF#0300331500000000`，期待 STD `013#0300331500004841`。这条查询读取节点 3 的 PMAX。
2. 确认电机已固定、空载且有行程后，选「零力矩」，将发送频率设为 100 Hz，勾选「连续发送」，再点「使能」。零力矩预设把 Kp、Kd 和力矩前馈设为零。已保存且启用齿槽表的 JM1 此时仍可能产生补偿电流。确认反馈和方向后再给小目标。
3. 结束时点「失能」并确认停止，之后断开。兼容镜像约 100 ms 未收到续期指令会关闭功率桥，恢复控制需要再次使能。GUI 的「标零」在 RS05/CyberGear 兼容镜像上是临时 RAM 零，需先失能并静止。DM4310 镜像收到标零帧会失能，持久机械零位在原生 V1 下设置。

无硬件时可以用下列命令验证界面和帧收发，启动后点「连接」。内置模拟从站用于联调界面。

```powershell
python sdk/python/joint_gui.py --interface virtual --channel vcan0 --protocol RS05 --sim
```

## 按报文联调其余六款

使用 CAN 分析仪或 `python-can`，通道设为经典 CAN 1 Mbit/s。以下均以已经设为节点 3 的 JM1 为例。STD 表示标准帧，EXT 表示扩展帧。发送前核对镜像、ID 类型和 DLC。

- **AK60-6**。发送 STD `003#FFFFFFFFFFFFFFFD`，在 STD `003` 读失能状态。`FC` 使能，`FD` 停机。MIT 力矩范围 ±15 Nm，速度范围 ±45 rad/s。
- **RMD-X**。发送 STD `143#9C00000000000000`，期待 STD `243` 回应。零 `A1` 用于握手，后续 `A1` 指令为直接 Iq，`80` 或 `81` 关桥。
- **LK-MF**。发送同一条 STD `143#9C00000000000000`，期待 STD `143` 回应。`88` 使能，`A1` 指令为直接 Iq，`80` 或 `81` 关桥。RMD-X 与 LK-MF 的电流定标不同。
- **HighTorque**。发送 DLC 2 的 EXT `00008003#1701`，期待 EXT `00000300` 回应且数据以 `27 01` 开头。发送 `05 13 00 00` 握手，后续 `05 13` 带有符号小端力矩，定标 0.01 Nm/LSB。发送 `01 00 00` 关桥。
- **DJI C610、C620**。连接后先监听 STD `203` 周期反馈。节点 3 收组 ID `0x200` 的 D4–D5，本槽为大端有符号直接电流值。先给本槽零做握手，后续按对应型号定标下发。给本槽零可停止。在共享总线上，由同一发送端生成组帧。

Windows 使用 PCAN 时，可先用 `python -m pip install python-can` 安装依赖，再把下面的示例保存为 `probe.py` 并运行 `python probe.py`。修改 `profile` 以匹配镜像。RMD-X 和 LK-MF 发送 `9C` 查询，DJI 分支只监听。

```python
import time
import can

profile = "rmd-x"  # rmd-x, lk-mf, c610, c620
reply_id = {"rmd-x": 0x243, "lk-mf": 0x143, "c610": 0x203, "c620": 0x203}[profile]
with can.Bus(interface="pcan", channel="PCAN_USBBUS1", bitrate=1_000_000) as bus:
    if profile in ("rmd-x", "lk-mf"):
        bus.send(can.Message(arbitration_id=0x143, is_extended_id=False,
                             data=bytes.fromhex("9C00000000000000")))
    deadline = time.monotonic() + 1.0
    while (remaining := deadline - time.monotonic()) > 0:
        frame = bus.recv(remaining)
        if (frame and not frame.is_extended_id and not frame.is_error_frame
                and frame.arbitration_id == reply_id and len(frame.data) == 8):
            print(frame)
            break
    else:
        raise SystemExit("指定节点未返回预期帧")
```

2026-11-23 在单台空载 JM1 节点 3 上，用 PCAN 验证了九款兼容镜像的正反小指令、停发关桥和重新握手。原厂主控互通和多机带载仍需对应台架。本机 `firmware/build/hil-20261123/` 中的脚本与日志属于本地构建产物，新克隆可使用本节的连通检查复测。
