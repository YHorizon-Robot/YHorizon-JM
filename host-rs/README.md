# YHorizon-JM 上位机

本目录是上位机 0.3.0，用 Rust 和 egui 写成，Windows、Linux、macOS 都能编译。生成的程序是 `yfoc-host`。在本目录执行：

```powershell
cargo build --release -p yfoc-host
.\target\release\yfoc-host.exe
```

没有参数时打开图形界面。命令行可以查询、扫描和发送运动指令：

```powershell
.\target\release\yfoc-host.exe --list-can
.\target\release\yfoc-host.exe --interface pcan --channel PCAN_USBBUS1 --scan
.\target\release\yfoc-host.exe --interface pcan --channel PCAN_USBBUS1 --id 1 --status
.\target\release\yfoc-host.exe --interface socketcan --channel can0 --id 1 --run --pos 0 --vel 10
```

Windows 打开 PCAN 前要安装 [PCAN-Basic](https://www.peak-system.com/PCAN-Basic.239.0.html)，让 `PCANBasic.dll` 能被找到。Linux 的 SocketCAN 波特率由系统设置，例如 `sudo ip link set can0 up type can bitrate 1000000`。slcan 用 115200 的串口，CAN 波特率在打开时下发。`usb2can`、Kvaser、Vector 会在界面里标成未实现。

界面上方是总线总览表，一台电机一行，显示状态、故障、模式、位置、速度、Iq、母线电压、反馈频率和当前活动。行尾的「停止」「清故障」直接作用于该行，点击行其余部分选中节点。下方左侧是选中节点的运行控制（速度、位置、Motion、电流），外环不符时自动先发 `SET_CONTROL_MODE` 并等 ACK；右侧是增益、电流环、管理、信息四页。示波器可以同时勾选多台。顶部还有「烧录」「校准与辨识」「多轴同步」。

总览栏可调三个轮询频率：状态（默认 2 Hz）、电流实测（默认关闭，看电流曲线时打开）、空闲反馈（默认 10 Hz，只对 1.3 固件且未在循环的节点发 `GET_FEEDBACK`）。

「停止」停掉本节点循环；1.3 固件还会发 `DISABLE` 关桥。「全部停止」或 Esc 停掉所有循环，直接电流节点补发 0 A，再向 `0x200` 广播 `DISABLE`；断开连接时同样处理。1.2 及更早的固件没有 `DISABLE`，停止只是不再发送，电机保持最后一帧目标。收到故障状态（包括 1.3 的主动故障上报）时，上位机停止该节点的发送。

1.4 固件：能力 bit6 表示焊了 NTC，状态里的温度有效，过温警告按原协议 bit0 显示为「过温」。能力 bit5 表示双编码器板的多圈未建立警告占用 Byte7 bit4，总览表用黄色显示「多圈未建立」。警告不会让上位机停止发送。

## 电流内环

连接或改变节点 ID 时先查询设备能力和状态。支持电流协议的固件（能力 bit3，1.2 起）继续返回两页配置：电流限幅、标幺限幅、带宽、PI Kp/Ki 和环频率。配置查询是只读的。校准与辨识页可测量本机 R/L、整定并保存 PI；有效自动整定记录优先于板级初值，最大电流由固件配置。

配置查询先读取两页，收到完整配置后再查询状态与电流，避免一次突发超过设备发送邮箱容量。缺失页在 1 秒后重试一次；仍超时则清除不完整配置并恢复刷新按钮，可手动重试。

Current 页可以切换电压／电流内环。切换先停止上位机循环；成功后固件清空实时指令并按新内环的默认值复位外环增益，上位机清空相关输入，并重新读取状态和配置。Motion 增益清零，电流内环的速度增益清零，位置增益恢复固件默认值；可在 Gains 页读取。之后需要按新内环重新整定和发送指令。内环选择是运行时设置，复位后使用固件默认值。

直接输入电流时，必须已确认电流内环且完整收到当前设备配置。上位机按 `t_ref = I_command × V_limit / I_limit` 换算，允许范围为设备限幅与 20 A 的较小值。当前固件默认 20 A、0.45 pu，对应约 44.444 A/pu；软件不固定此比例。输入是控制方向的电流指令，电角度坐标下的有符号 `Iq_ref` 以电流反馈帧为准。

发送顺序为 `SET_CONTROL_MODE(MOTION)` → 等待成功 ACK → `SET_GAINS(MOTION, 0, 0, 0)` → 等待成功 ACK → 发送电流前馈。错误或超时会取消后续运控帧。切换节点立即清除能力、内环和比例缓存，不会沿用前一块板卡的配置。

电流入口的“发送一次”会由固件持续保持，直到新指令或停止。直接电流操作后点击“停止循环”、切换控制动作或断开连接时，上位机会先取消尚未发送的实时帧，再同步发送 0 A。也可使用“发送 0 A 并停止循环”。开始循环时固定发送点击按钮那一刻的电流值，修改输入后重新点击发送。普通 Motion／Velocity／Position 页的“停止循环”仍仅停止发送；这些模式的停止目标由用户指定。

## 反馈

- `t_ref` 是标幺指令，不是实测电流或实测转矩。
- Current 页显示实测 `Id`、实测 `Iq`、指令 `Iq_ref`（A）以及 PI 输出 `Vq`（pu）。每次有效运动反馈同时带电流帧，也可手动刷新。
- 示波窗口右键可以添加 `Id`、`Iq`、`Iq_ref`、`Vq`；未知电流没有伪造的零值。电流和运动帧分别按到达时间记录，“按帧”统计的是这两类采样点。
- 电流采样失效显示 `CURRENT_SAMPLE_FAULT`，过流显示 `OVER_CURRENT`。新固件每秒查询一次状态；检测到故障后停止直接电流循环。

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

# 已确认电流内环后，只发一次 0 A，收到反馈退出
yfoc-host --interface socketcan --channel can0 --id 1 --current 0 --once

# 运控前馈单位是输出轴 Nm；先确认模式与增益 ACK
yfoc-host --interface socketcan --channel can0 --id 1 --kp 0 --kd 0 --ff 0 --once

# 完全被动监听，不发送设备查询
yfoc-host --interface socketcan --channel can0 --id 1 --listen
```

`--current` 不会默默切换内环；需要设备已经处于电流内环，或显式加 `--inner-loop current`。CLI 的直接电流操作在正常退出、Ctrl+C 或异常退出前都会尝试发送 0 A；因此 `--current ... --once` 是收到一次反馈后归零退出。电流配置/状态选项在旧固件上明确报不支持。`--listen` 不主动查询，因此无法获知旧帧中未提供的内环信息。

本上位机只使用 Motor Protocol V1.0，烧录页只提供 YH_JM。RS05 / DM4310 联调界面在 `sdk/python/joint_gui.py`，协议笔记在 `docs/`。
