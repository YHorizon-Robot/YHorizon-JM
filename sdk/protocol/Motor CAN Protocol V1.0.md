# Motor CAN Protocol V1.0 — YHorizon-JM 固件 1.6

[中文](Motor%20CAN%20Protocol%20V1.0.md) | [English](Motor%20CAN%20Protocol%20V1.0.en.md)

下文保留原协议正文；标注“本固件”的段落以及本节扩展定义描述本项目的实际行为。原协议的转矩、ENABLE 条款不能直接套用本固件；DISABLE 自 1.3 起实现，语义见下文「运控扩展」。

## 本固件实现标注

固件 1.6 新增组同步协议。预装与就绪使用管理操作码 `0x40`～`0x44`，公共同步 ID 为 `0x080`，带周期号的每轴反馈使用 `0x540 + ID`。组运动有独立的同步超时保护。帧格式见[多轴同步位置控制](../../docs/multi-axis.md)。

本固件默认 **20 kHz 电流内环**，电压内环仍可选择。4 kHz 的 Velocity／Position 外环仍输出归一化指令 `t_ref`。运控模式的 Kp、Kd 和力矩前馈按输出轴牛·米接收：`t_ref = T_Nm × V_limit / (Kt × I_limit × 减速比)`，固件再用 `t_ref` 算电流。前馈和反馈力矩都是 `int16 × 0.001 Nm`。速度、位置模式的增益单位不变。

- 常规电流内环使用 `Iq_ref = closed_loop_dir × t_ref × I_limit / V_limit` 和 `Id_ref=0`。正式固件的配置上限为 20 A、0.45 pu，主机从设备读取实际比例。启用齿槽表时，速度、位置和 Motion（含零增益纯力矩）另加位置相关的电流前馈。直接 Iq 跳过查表。内部电流指令按斜坡变化。
- 电压内环：`Vq = closed_loop_dir × t_ref`；开环编码器校准始终使用电压。
- 上电校准／加载成功后状态为 RUNNING，桥保持关闭，收到新实时命令才恢复。没有 ENABLE，也没有运控命令超时；非零指令发一次后会持续保持，直到新命令、DISABLE 或故障。
- `SET_CONTROL_MODE`、`SET_INNER_LOOP`、写 Flash 都先关闭桥、清目标和积分；恢复需新实时命令、有效采样及中性占空比锁存。切换内环还把外环增益重置为对应默认值。电流内环速度 Kp/Ki 默认 0，须单独整定；直接电流入口不依赖速度／位置环。
- `SAVE_USER_PARAMS` 保存 Vel/Pos 增益及其内环和缩放标记，不保存运行模式、Motion 增益或电流 PI。NV V4 兼容读取 V1–V3，保留编码器校准、节点和零点；旧电压增益不会自动加载到电流内环。
- 同一非实时 Command+Sequence 重发缓存应答，分页查询须使用不同 Sequence。
- 固件 1.5 按 PWM 时序执行试验，主机通过状态请求续期，普通运动指令在试验期间由固件拦截。写 Flash 期间关闭功率桥并暂停 CAN。

### 校准与辨识扩展（固件 1.5）

设备能力 bit7 表示支持试验协议。上位机将 `0x30` 至 `0x37` 发送到 `0x200 + ID`，设备在 `0x480 + ID` 返回结果。该扩展负责试验启动、状态与心跳、样本读取、停止、参数暂存与 CRC 提交。拟合与候选评分在主机执行，参数导出复用读取命令。详见[校准与辨识操作及协议说明](../../docs/commissioning.md)。

### 电流扩展命令与反馈

所有字段均为小端，标准 CAN，DLC=8，ID=1…63。

`GET_DEVICE_INFO (0x09)` 成功 ACK 的 Byte4=板卡号，Byte5=能力，Byte6/7=固件主/次版本。能力 bit0=第二编码器，bit1=电流采样，bit2=工厂固件，bit3=本节电流协议，bit4=运控扩展，bit5=多圈位置警告，bit6=NTC 在位，bit7=校准与辨识扩展。当前正式固件版本为 1.6。主机以 bit3 判断安培反馈与内环状态是否可用。

`SET_INNER_LOOP (0x0A)` 请求 ID=`0x200+ID`：Byte0=0x0A，Byte1=Sequence，Byte2=0 电压／1 电流，Byte3…7=0。仅 RUNNING 接受；成功在 `0x280+ID` 回标准 ACK，Byte6 回所选内环。选择仅保存在 RAM，重启使用编译默认。

`GET_CURRENT_CONFIG (0x22)`：Byte0=0x22，Byte1=Sequence，Byte2=页码 0/1/2，余字节为 0。成功响应 ID=`0x440+ID`：Byte0=页码，Byte1=Sequence，Byte2…7 为三个 uint16。页 2：Byte2…3=`Kt × 1000`（Nm/A），Byte4…5=`减速比 × 100`，Byte6…7 为扩展能力字，bit0=支持独立 Iq 实时指令。旧固件此字为零。

- 第 0 页 Byte2…3 为 `I_limit × 1000`（mA），Byte4…5 为 `V_limit × 10000`（pu），Byte6…7 为设计带宽（Hz）。固件 1.5 在 R/L 和电流 PI 整定标志均有效时由保存的 Ki 与 Rs 反算带宽，其余情况报告板卡默认值。
- 第 1 页 Byte2…3 为当前 `Kp × 1000`（Kp 单位 V/A），Byte4…5 为当前 Ki（V/(A·s)），Byte6…7 为电流环执行频率（Hz）。

**独立 Iq 指令**使用 `0x4C0 + ID`，Byte0…1 是有符号小端 int16，单位 mA，Byte2…7 全零。要求节点已校准、RUNNING、Motion 外环及电流内环。幅值超出设备电流上限或保留字节非零时丢弃。有效命令和 Motion 共用原有实时反馈 `0x300 + ID` 和停机逻辑，同一接收批次最后一条有效目标生效。以控制方向给定 Iq，闭环方向转换、斜坡和最终电流限幅继续生效。

节点 1 的 `101#000000000000E803` 在 Kp=Kd=0 时请求输出轴 +1 Nm。有效齿槽表会在低速参与补偿。`4C1#E803000000000000` 请求直接 +1 A。`4C1#0000000000000000` 请求直接 0 A。主动下发 0 Nm 时仍可有补偿电流，关桥请发 DISABLE。旧上位机将 A 换成 Nm 后发 Motion，固件会按力矩处理，使用直接电流时须同时更新上位机。

`GET_CURRENT_STATE (0x23)`：Byte0=0x23，Byte1=Sequence，余字节为 0。成功响应 ID=`0x400+ID`，四个 int16 依次为：Byte0…1=Id（mA），Byte2…3=Iq（mA），Byte4…5=Iq_ref（mA），Byte6…7=Vq（0.0001 pu）。无 ACK，也不回 Sequence；该帧只在收到本命令时发送，不随运动反馈上报。Id/Iq 是最近一次有效采样，异常时结合 GET_STATUS 故障位判断，不能把故障期间保持的数值当成新样本。

`GET_STATUS` 的 Byte7：bit0…4 Warning，bit5…6 Control Mode，**bit7 Inner Loop**（0 电压，1 电流）。此解释仅适用于已确认能力 bit3 的设备；旧固件 bit7=0 不足以推断电压内环。Warning 位的含义见「位置警告」。

### 运控扩展（固件 1.3，能力 bit4）

- `DISABLE (0x02)`：请求 ID=`0x200+ID`，Byte0=0x02，Byte1=Sequence，余字节为 0。关桥、清零积分并清除保持的实时目标，与 `SET_CONTROL_MODE` 后的状态相同；Motor State 仍为 RUNNING（FAULT 时不变），外环模式和增益不变。之后收到与当前模式匹配的新实时命令即恢复输出，不需要 ENABLE。成功回标准 ACK。
- **广播停止**：`CAN ID = 0x200`（即节点号 0），Byte0=0x02，Byte1…7=0。所有 1.3 节点同时执行 DISABLE，**不回 ACK**，避免整条总线同时应答。广播只接受 DISABLE，其他内容丢弃。旧固件的硬件过滤器收不到这帧。
- `GET_FEEDBACK (0x11)`：Byte0=0x11，Byte1=Sequence，余字节为 0。回一帧 `0x300+ID` Motion Feedback，内容与实时命令的反馈相同，无 ACK、不经重发缓存，任何模式和状态下都应答。用于在不发运动命令时读取位置，例如手推电机。
- **故障主动上报**：伺服层出现新的故障位时，立即主动发送一帧 `0x380+ID`，格式与 GET_STATUS 应答相同。只在故障位新增时发送一次，不周期上报；已闩锁的同一故障不会重复上报。

### 温度与位置警告（固件 1.4）

PA3 接 10 kΩ / SDNT1608X103F3950FTF（NTC 在下）。有些板子没焊 NTC（PA3 被上拉拉满或悬空），上电会主动探测：先推挽拉低再拉高，两次读数接近且都在量程内才认为在位。没焊则能力 bit6=0，温度字段填 0，不置温度警告、不过温跳闸。运行中读数跑出量程（松脱/短路）只当作读不到，撤销温度警告，不跳闸。

| Byte7 位 | 名称 | 条件 | 含义 |
|---|---|---|---|
| bit0 | `MOTOR_TEMPERATURE_WARNING` | 能力 bit6，板温连续约 1 s ≥ 80 °C | 与原协议同位。≥ 100 °C 另闩锁 `OVER_TEMPERATURE` 故障；降到 75 °C 以下才撤销警告、才允许清过温故障。 |
| bit4 | `ABS_POSITION_UNKNOWN` | 能力 bit5，双编码器板 | 上电游标没定出圈数。反馈只保证在零点所在那一圈电机轴内。原协议该位叫 `POSITION_LIMIT_WARNING`，本固件未实现限位；上位机须确认能力 bit5 后再按本表解释。 |

- 出现新的警告位时主动发一帧 `0x380+ID`，规则与故障主动上报相同。
- 游标只在上电定圈（及定圈失败后关桥无指令时的补定圈）时读取；运行中位置由电机编码器累加，不再读第二编码器。1.3 及更早在游标与累计位置相差超过半圈时会改写累计位置，实机上跳一电机圈。
- `ABS_POSITION_UNKNOWN` 置位期间 `SET_ZERO` 返回 `INVALID_STATE`。关桥且无实时指令时，主循环每毫秒读游标，连续 20 次解出同一整圈修正才补上并撤销该位。
- 上电取离保存零点最近的等价角度，唯一区间 31 圈电机轴。减速比 8 时，输出轴约 ±1.94 圈，覆盖「设零后正反各一圈」。正好半圈落在 31 圈周期的中点，会折到另一侧，半圈以内可以恢复。
- 游标零偏由 `START_CALI` 写入 NV。旧记录没有零偏，升级后须重新校准一次。

### 已实现故障及恢复

- bit0 OVER_TEMPERATURE：板温连续约 1 s ≥ 100 °C（仅能力 bit6）。清故障还要求当前温度 < 75 °C。
- bit1 OVER_CURRENT：任一测得相电流（含重建 C 相）绝对值 >15 A，立即关桥。
- bit3 UNDER_VOLTAGE：闭合电流 PI／重新使能前母线低于 8 V，或连续约 10 ms 读到母线低于 8 V。单次转换失败仍沿用上一笔，超过 100 ms 没有新采样才视为无效。
- bit4 ENCODER_FAULT：连续 4 个 PWM 周期编码器读取失败。MT6701 在角度计数为 0 且状态位为 0 时整帧全零；1.4 起上一帧有效读数在零点 ±64 个计数内时接受全零帧，1.3 及更早一律判无效，低速过零或停在零点会误报本故障。数据线卡低时若上一帧恰在该窗口内则无法区分。
- bit10 CALIBRATION_FAULT：校准失败。
- **bit11 CURRENT_SAMPLE_FAULT**：零点校准无效、ADC 轨边／非有限样本，或连续 2 个 PWM 周期没有新电流样本。

故障闩锁且关闭 PWM。运行时故障的 CLEAR_FAULT 要求电流校零有效、新采样有效、编码器恢复、有效母线以及三相电流绝对值均不超过 1 A；过温另要求温度已降到警告清除点以下。恢复后仍等新命令。校准失败清除后保持 IDLE，必须重新成功校准。软件采样保护不替代驱动器的硬件短路保护。过压、堵转与驱动器专用故障尚未实现；新故障位会按「运控扩展」主动上报一帧状态。

整定值、采样窗口、离线验证范围及首次上板步骤见 [电流环说明](../../docs/current-loop.md)。

---

## 1. 基础定义

```text
CAN              CAN 2.0 Standard
Identifier       11 bit
DLC              8 Byte
Baudrate         1 Mbps
Byte Order       Little Endian
Motor ID         1 ~ 63
```

CAN ID 分配：

| CAN ID | 方向 | 功能 |
|---|---|---|
| `0x100 + ID` | 主机 → 电机 | Motion Command |
| `0x140 + ID` | 主机 → 电机 | Velocity Mode Command |
| `0x180 + ID` | 主机 → 电机 | Control Gains |
| `0x1C0 + ID` | 主机 → 电机 | Position Mode Command |
| `0x200 + ID` | 主机 → 电机 | Management Command |
| `0x280 + ID` | 电机 → 主机 | Command ACK |
| `0x300 + ID` | 电机 → 主机 | Motion Feedback |
| `0x380 + ID` | 电机 → 主机 | Status Response |
| `0x3C0 + ID` | 电机 → 主机 | Encoder Calibration Report |

> **本固件：** 三模式均已实现。`0x380` 作为 `GET_STATUS` 应答；1.3 起出现新故障位、1.4 起出现新警告位时也主动发一帧。

### Control Mode

```text
0x00  MOTION_MODE
0x01  VELOCITY_MODE
0x02  POSITION_MODE
```

Control Mode 由 `SET_CONTROL_MODE` 管理命令显式切换。上电默认模式为 `MOTION_MODE`。

> **本固件：** 三模式均可运行。上电默认 `MOTION_MODE`。`SET_CONTROL_MODE` 在 `RUNNING` 下切换；`FAULT` 返回 `INVALID_STATE`。

---

# 2. 实时控制命令

电机支持以下三种实时控制模式：

| 控制模式 | CAN ID | 控制量 |
|---|---|---|
| 运控模式（Motion） | `0x100 + ID` | 目标位置、目标速度、转矩/电压前馈 |
| 速度模式（Velocity） | `0x140 + ID` | 目标速度 |
| 位置模式（Position） | `0x1C0 + ID` | 目标位置、最大速度 |

Control Mode 必须先通过 `SET_CONTROL_MODE` 选择。驱动器只接受与当前 Control Mode 匹配的实时控制命令：

| 当前 Control Mode | 有效实时控制命令 |
|---|---|
| `MOTION_MODE` | Motion Command |
| `VELOCITY_MODE` | Velocity Mode Command |
| `POSITION_MODE` | Position Mode Command |

收到其他模式的实时控制命令时，驱动器不执行该帧，也不返回 Motion Feedback。三种有效实时控制命令均使用 `Motion Feedback (0x300 + ID)` 作为反馈。

协议不设置实时命令超时保护。进入 RUNNING 后，驱动器持续保持最后一帧有效实时控制命令，直到收到同模式的新命令、DISABLE、进入 FAULT 或掉电。上位机停止周期控制前必须主动发送 DISABLE。

> **本固件：** 1.3 起支持 `DISABLE` 与 `0x200` 广播停止；1.2 及更早无 `DISABLE`，停止运动需自行发目标（例如当前位置、速度 0、增益 0），或断电。`CLEAR_FAULT` 清校准故障后 PWM 关闭，此时 Motion 不执行。

## 2.1 Motion Command

```text
CAN ID = 0x100 + MotorID
Direction = Master → Motor
DLC = 8
```

### Payload

| Byte | 数据 | 类型 | 分辨率 |
|---|---|---|---|
| 0~3 | Position Desired | `int32` | `0.0001 rad/LSB` |
| 4~5 | Velocity Desired | `int16` | `0.1 rad/s/LSB` |
| 6~7 | Torque / Voltage Feedforward | 取决于固件输出模式 | 见下文 |

```text
Byte0  Position[7:0]
Byte1  Position[15:8]
Byte2  Position[23:16]
Byte3  Position[31:24]

Byte4  Velocity[7:0]
Byte5  Velocity[15:8]

Byte6  TorqueFF[7:0]
Byte7  TorqueFF[15:8]
```

换算：

```text
Position(rad)   = PositionRaw × 0.0001
Velocity(rad/s) = VelocityRaw × 0.1
```

Byte6~7 的含义由固件输出模式决定：

| 固件输出模式 | 类型 | 取值/分辨率 | 换算 |
|---|---|---|---|
| 转矩模式 | `int16` | `0.001 Nm/LSB` | `TorqueFF(Nm) = Raw × 0.001` |
| 电压模式 | `uint16` | 归一化电压 `0.0 ~ 1.0` | `VoltageFF = Raw / 65535` |

电压模式中的 `VoltageFF` 是归一化电压指令：

```text
Raw = 0       → VoltageFF = 0.0
Raw = 32768   → VoltageFF ≈ 0.5
Raw = 65535   → VoltageFF = 1.0
```

其实际电压由固件的母线电压、调制方式和电压限幅决定。主机必须根据目标固件的输出模式编码 Byte6~7，不能将转矩模式的 `int16` 数值直接用于电压模式。

> **本固件：** 运控前馈按输出轴 `int16 × 0.001 Nm`。速度按协议 `int16 × 0.1 rad/s` 接收（约 `±3276.7`），超出则钳位，仍执行并回 Feedback。换算后的 `t_ref` 再限幅 `CFG_V_LIMIT`。

转矩模式控制律：

\[
Torque_{cmd}
=
K_p(Position_{des}-Position)
+
K_d(Velocity_{des}-Velocity)
+
Torque_{ff}
\]

电压模式控制律：

\[
Voltage_{cmd}
=
K_p(Position_{des}-Position)
+
K_d(Velocity_{des}-Velocity)
+
Voltage_{ff}
\]

`Voltage_cmd` 最终由固件限制在允许的归一化电压范围内。

### 应答

正常情况下，电机处理有效 Motion Command 后返回一帧 Motion Feedback。

```text
Motion Command
0x100 + ID
      ↓
Motion Feedback
0x300 + ID
```

Motion Command **没有额外 ACK**，`0x300 + ID` 即为它的反馈帧。一次接收处理中存在多帧实时控制命令时，按照第 19 节的实时帧合并规则处理。

---

## 2.2 Velocity Mode Command

> **本固件：** 已实现。速度 PI 输出归一化指令，`e_v = v_des - v_act`，`u = Kp e_v + Ki ∫e_v dt`，输出限幅 `±CFG_V_LIMIT`。积分钳位到 `±V_LIMIT`。速度超出协议范围则钳位后仍执行；Reserved 非 0 的帧视为无效。

速度模式使用电机内部速度环，控制量为 4 Byte 目标速度。

```text
CAN ID = 0x140 + MotorID
Direction = Master → Motor
DLC = 8
```

### Payload

| Byte | 数据 | 类型 | 分辨率 |
|---|---|---|---|
| 0~3 | Velocity Desired | `int32` | `0.1 rad/s/LSB` |
| 4~7 | Reserved = 0 | `uint32` | - |

```text
Byte0  Velocity[7:0]
Byte1  Velocity[15:8]
Byte2  Velocity[23:16]
Byte3  Velocity[31:24]
Byte4~7 = 0
```

换算：

```text
Velocity(rad/s) = VelocityRaw × 0.1
VelocityRaw     = round(Velocity(rad/s) / 0.1)
```

正值和负值分别表示两个旋转方向，具体正方向由电机安装方向和固件配置确定。

### 应答

正常情况下，电机处理有效 Velocity Mode Command 后返回一帧：

```text
Motion Feedback
CAN ID = 0x300 + MotorID
```

Velocity Mode Command 没有额外 ACK。一次接收处理中存在多帧实时控制命令时，按照第 19 节的实时帧合并规则处理。

---

## 2.3 Position Mode Command

> **本固件：** 已实现。位置外环 PID 生成 `v_ref`（限幅 `±MaximumVelocity`，且不超过协议速度上限约 `3276.7 rad/s`），再交给速度 PI 内环（使用 `VELOCITY_MODE` Gains）。位置 D 项为 `-Kd·v_act`。外环积分对 `v_ref` 限幅钳位；归一化指令饱和时冻结位置外环积分。

位置模式采用位置外环、速度内环。控制量为 4 Byte 目标位置和 4 Byte 最大速度。

```text
CAN ID = 0x1C0 + MotorID
Direction = Master → Motor
DLC = 8
```

### Payload

| Byte | 数据 | 类型 | 分辨率 |
|---|---|---|---|
| 0~3 | Position Desired | `int32` | `0.0001 rad/LSB` |
| 4~7 | Maximum Velocity | `uint32` | `0.1 rad/s/LSB` |

```text
Byte0  Position[7:0]
Byte1  Position[15:8]
Byte2  Position[23:16]
Byte3  Position[31:24]

Byte4  MaxVelocity[7:0]
Byte5  MaxVelocity[15:8]
Byte6  MaxVelocity[23:16]
Byte7  MaxVelocity[31:24]
```

换算：

```text
Position(rad)        = PositionRaw × 0.0001
MaximumVelocity(rad/s) = MaxVelocityRaw × 0.1
```

`Position Desired` 使用与 Motion Command 和 Motion Feedback 相同的连续多圈机械角度。`Maximum Velocity` 是绝对值上限，不表示旋转方向；运动方向由当前位置与目标位置之间的误差决定。

### 应答

正常情况下，电机处理有效 Position Mode Command 后返回一帧：

```text
Motion Feedback
CAN ID = 0x300 + MotorID
```

Position Mode Command 没有额外 ACK。一次接收处理中存在多帧实时控制命令时，按照第 19 节的实时帧合并规则处理。

---

# 3. Motion Feedback

```text
CAN ID = 0x300 + MotorID
Direction = Motor → Master
DLC = 8
```

## Payload

| Byte | 数据 | 类型 | 分辨率 |
|---|---|---|---|
| 0~3 | Position Actual | `int32` | `0.0001 rad/LSB` |
| 4~5 | Velocity Actual | `int16` | `0.1 rad/s/LSB` |
| 6~7 | Torque Actual | `int16` | `0.001 Nm/LSB` |

> **本固件：** Byte6~7 填运控指令对应的输出轴力矩，分辨率 `0.001 Nm/LSB`。无转矩传感器，不是实测力矩。位置为连续多圈机械角，与原文一致。int16 最大约 ±32.767 Nm。

```text
Byte0  Position[7:0]
Byte1  Position[15:8]
Byte2  Position[23:16]
Byte3  Position[31:24]

Byte4  Velocity[7:0]
Byte5  Velocity[15:8]

Byte6  Torque[7:0]
Byte7  Torque[15:8]
```

换算：

```text
Position(rad)   = PositionRaw × 0.0001
Velocity(rad/s) = VelocityRaw × 0.1
Torque(Nm)      = TorqueRaw × 0.001
```

位置采用连续多圈机械角度：

```text
Position = 0                  机械零点
Position = +2π                正向 1 圈
Position = +20π               正向 10 圈
Position = -2π                反向 1 圈
```

---

# 4. Control Gains

> **本固件：** 三组 Gains 均接受，分辨率按原文电压模式。MOTION 的 Ki、VELOCITY 的 Kd 必须为 0。有效范围见 `config.h`。ACK 不改变当前 Control Mode。

根据 Control Mode 分别设置运控模式的 `Kp/Kd`、速度模式速度环的 `Kp/Ki`，以及位置模式位置外环的 `Kp/Ki/Kd`。

```text
CAN ID = 0x180 + MotorID
Direction = Master → Motor
DLC = 8
```

## Payload

| Byte | 内容 |
|---|---|
| 0 | Control Mode |
| 1 | Sequence |
| 2~3 | Kp `uint16` |
| 4~5 | Ki `uint16` |
| 6~7 | Kd `uint16` |

Control Mode 使用第 1 节的统一定义：

```text
0x00  MOTION_MODE
0x01  VELOCITY_MODE
0x02  POSITION_MODE
```

所有字节均有固定定义，不再包含 Reserved 字段。对于当前控制算法未使用的增益，主机必须发送 0，驱动器保存为 0：

| Control Mode | Kp | Ki | Kd |
|---|---|---|---|
| `MOTION_MODE` | 使用 | 必须为 0 | 使用 |
| `VELOCITY_MODE` | 使用 | 使用 | 必须为 0 |
| `POSITION_MODE` | 使用 | 使用 | 使用 |

Control Mode 非法，或者要求为 0 的增益不为 0 时，返回 `PARAMETER_OUT_OF_RANGE`，原有参数保持不变。

### Motion Gains

运控模式的 `Kp` 和 `Kd` 原始编码不变，其物理单位由固件输出模式决定：

| 参数 | 类型 | 转矩模式分辨率 | 电压模式分辨率 |
|---|---|---|---|
| Kp | `uint16` | `0.01 Nm/rad/LSB` | `0.01 pu/rad/LSB` |
| Kd | `uint16` | `0.001 Nm·s/rad/LSB` | `0.001 pu·s/rad/LSB` |

> **本固件：** 运控 Kp、Kd 按输出轴牛·米，分辨率与转矩模式相同。进入电流环前换成标幺。速度和位置模式仍用各自的标幺单位。

### Velocity PI Gains

速度模式使用内部速度 PI，速度误差为：

\[
e_v = Velocity_{des} - Velocity
\]

控制器输出为转矩模式下的转矩指令，或者电压模式下的归一化电压指令：

\[
Output_{cmd} = K_{p,v}e_v + K_{i,v}\int e_vdt
\]

| 参数 | 类型 | 转矩模式分辨率 | 电压模式分辨率 |
|---|---|---|---|
| Velocity Kp | `uint16` | `0.001 Nm/(rad/s)/LSB` | `0.001 pu/(rad/s)/LSB` |
| Velocity Ki | `uint16` | `0.001 Nm/rad/LSB` | `0.001 pu/rad/LSB` |

### Position PID Gains

位置模式采用位置 PID 外环和速度 PI 内环。位置外环根据位置误差生成速度内环的目标速度：

\[
e_p = Position_{des} - Position
\]

\[
Velocity_{ref}
=
K_{p,p}e_p
+
K_{i,p}\int e_pdt
+
K_{d,p}\frac{de_p}{dt}
\]

`Velocity_ref` 最终限制在 Position Mode Command 给定的 `±MaximumVelocity` 范围内，再交给速度 PI 内环控制。

| 参数 | 类型 | 分辨率 |
|---|---|---|
| Position Kp | `uint16` | `0.01 s⁻¹/LSB` |
| Position Ki | `uint16` | `0.001 s⁻²/LSB` |
| Position Kd | `uint16` | `0.001/LSB` |

设置 `POSITION_MODE` Gains 只修改位置外环参数，不修改 `VELOCITY_MODE` 的速度 PI 内环参数。位置模式运行前，主机应分别配置 `VELOCITY_MODE` 和 `POSITION_MODE` Gains。

所有参数对应的原始编码范围均为：

```text
Raw = 0 ~ 65535
```

固件可以根据电机、功率级和控制周期设置更小的有效参数范围。驱动器必须先校验一帧中的全部参数，再一次性更新对应控制器的参数；任一参数越界时返回 `PARAMETER_OUT_OF_RANGE`，且本帧参数全部不生效。

## 应答

需要 ACK：

```text
Control Gains
0x180 + ID
      ↓
Command ACK
0x280 + ID
```

ACK 中：

```text
Command = 0x20   // SET_GAINS
Sequence = 请求中的 Sequence
```

ACK 不改变当前 Control Mode。Control Gains 中的 Control Mode 只用于选择要修改的参数组，实际模式切换必须使用 `SET_CONTROL_MODE`。

SET_GAINS 只更新 RAM。速度/位置环要进 Flash 须另发 `SAVE_USER_PARAMS`。Motion Kp/Kd 不写入 NV，上电为 0，除非主机本会话已 SET。

## 4.1 GET_GAINS（本固件扩展）

读**当前 RAM**（不是 Flash 镜像）。上电后若 NV 用户增益的内环及缩放标记匹配，Vel/Pos 来自 Flash；Motion 仍为 0，除非主机刚 SET 过。三组都可 GET，便于核对。

请求：

```text
CAN ID = 0x200 + MotorID

Byte0 = 0x21
Byte1 = Sequence
Byte2 = Control Mode（与 SET_GAINS 相同：0=MOTION / 1=VELOCITY / 2=POSITION）
Byte3~7 = 0
```

成功：不再额外返回 Command ACK。应答与 SET_GAINS 同布局、同 LSB：

```text
CAN ID = 0x180 + MotorID
Direction = Motor → Master

Byte0 = Control Mode
Byte1 = Sequence
Byte2~3 = Kp uint16
Byte4~5 = Ki uint16
Byte6~7 = Kd uint16
```

Mode 非法或 Reserved 非 0 时，在 `0x280` 返回 `PARAMETER_OUT_OF_RANGE`。

---

# 5. Management Command

```text
CAN ID = 0x200 + MotorID
Direction = Master → Motor
DLC = 8
```

统一格式：

| Byte | 内容 |
|---|---|
| 0 | Command |
| 1 | Sequence |
| 2~7 | Arguments / Reserved |

Command 定义：

```text
0x01  ENABLE
0x02  DISABLE
0x03  SET_ZERO
0x04  CLEAR_FAULT
0x05  START_ENCODER_CALIBRATION
0x06  SET_CONTROL_MODE
0x07  SET_NODE_ID（本固件扩展）
0x08  SAVE_USER_PARAMS（本固件扩展）
0x09  GET_DEVICE_INFO（本固件扩展）
0x0A  SET_INNER_LOOP（本固件 1.2 扩展）
0x22  GET_CURRENT_CONFIG（本固件 1.2 扩展）
0x23  GET_CURRENT_STATE（本固件 1.2 扩展）
0x11  GET_FEEDBACK（本固件 1.3 扩展）

0x10  GET_STATUS
0x21  GET_GAINS（本固件扩展）
```

> **本固件：** `ENABLE` → `INVALID_COMMAND`；`DISABLE` 与 `GET_FEEDBACK` 自 1.3 起实现，见开头「运控扩展」。其余命令见各节标注。扩展命令 `0x07 SET_NODE_ID` 见 5.2，`0x08 SAVE_USER_PARAMS` 见 5.3，`0x09 GET_DEVICE_INFO` 见 5.4，`0x21 GET_GAINS` 见 4.1。

## 5.1 SET_CONTROL_MODE

显式切换驱动器当前使用的实时控制模式。

请求：

```text
CAN ID = 0x200 + MotorID

Byte0 = 0x06
Byte1 = Sequence
Byte2 = Control Mode
Byte3~7 = 0
```

Control Mode：

```text
0x00  MOTION_MODE
0x01  VELOCITY_MODE
0x02  POSITION_MODE
```

模式切换只允许在以下状态执行：

```text
DISABLED
READY
```

处于 RUNNING、FAULT 或 CALIBRATING 时返回：

```text
Result = INVALID_STATE
```

> **本固件：** 无 `DISABLED`/`READY`。`RUNNING` 下可切换 `MOTION`/`VELOCITY`/`POSITION`。切换后关闭桥、清零积分，并清除实时目标有效标志（此时桥关闭），直到收到与新模式匹配的有效实时命令。`FAULT` 时返回 `INVALID_STATE`。

切换成功后：

```text
保存新的 Control Mode
        ↓
清零新模式的积分器和微分器状态
        ↓
清除上一模式的实时目标有效标志
        ↓
保持当前 Motor State
        ↓
发送 ACK
```

Control Gains 参数不会因为切换模式而被清除。切换后必须收到一帧与新模式匹配的有效实时控制命令，电机才会使用新的实时目标。处于 READY 时，该帧同时使电机进入 RUNNING。

应答：

```text
CAN ID = 0x280 + MotorID

Command  = 0x06
Sequence = 请求中的 Sequence
Result   = OK / INVALID_STATE / PARAMETER_OUT_OF_RANGE
```

Control Mode 非法或 Byte3~7 不为 0 时返回 `PARAMETER_OUT_OF_RANGE`。

## 5.2 SET_NODE_ID（本固件扩展）

运行时修改 Motor ID，写入与校准共用的 Flash 页后软件复位。请求仍发到 **当前** `0x200+ID`。

```text
CAN ID = 0x200 + MotorID

Byte0 = 0x07
Byte1 = Sequence
Byte2 = New Motor ID（1~63）
Byte3~7 = 0
```

> **本固件：** `CALIBRATING` → `BUSY`。ID 非法或 Reserved 非 0 → `PARAMETER_OUT_OF_RANGE`。写 Flash 失败 → `INTERNAL_ERROR`，不复位。`RUNNING`/`FAULT` 可执行。成功时先在 **旧 ID** 的 `0x280` 回 ACK（Byte6=新 ID），约 20 ms 后 `NVIC_SystemReset`。新 ID 与当前相同则 ACK 且不复位。无有效校准时仍可只写 ID；之后校准 `CaliNv_Save` 会保留该 ID。PB1 长按 2 s 进入改 ID（短按 +1，再长按保存复位，10 s 无操作退出）。

应答：

```text
CAN ID = 0x280 + 旧 MotorID

Command  = 0x07
Sequence = 请求中的 Sequence
Result   = OK / BUSY / PARAMETER_OUT_OF_RANGE / INTERNAL_ERROR
Byte6    = New Motor ID（成功时）
```

## 5.3 SAVE_USER_PARAMS（本固件扩展）

把当前 RAM 里的速度环 `Kp/Ki` 和位置环 `Kp/Ki/Kd` 写入与校准共用的 Flash 记录。读-改-写，一个 CRC。不保存：Motion Kp/Kd、控制模式、目标位置/速度/前馈、使能态。多圈机械零点由 `SET_ZERO` 单独写入同一记录。

```text
CAN ID = 0x200 + MotorID

Byte0 = 0x08
Byte1 = Sequence
Byte2~7 = 0
```

> **本固件：** `CALIBRATING` → `BUSY`。Reserved 非 0 → `PARAMETER_OUT_OF_RANGE`。写 Flash 失败 → `INTERNAL_ERROR`。无有效校准记录时仍可只写增益；之后校准 `CaliNv_Save` 会保留它们。上电加载校准时若记录带增益标志，且内环及缩放标记匹配，才加载 Vel/Pos；否则使用对应内环默认值。

应答：

```text
CAN ID = 0x280 + MotorID

Command  = 0x08
Sequence = 请求中的 Sequence
Result   = OK / BUSY / PARAMETER_OUT_OF_RANGE / INTERNAL_ERROR
```

## 5.4 GET_DEVICE_INFO（本固件扩展）

查询板卡号、能力位和固件版本。成功时 `0x280` 的 Byte4~7 不再是故障码。

```text
CAN ID = 0x200 + MotorID

Byte0 = 0x09
Byte1 = Sequence
Byte2~7 = 0
```

成功应答：

```text
CAN ID = 0x280 + MotorID

Byte0 = 0x09
Byte1 = Sequence
Byte2 = OK
Byte3 = Motor State
Byte4 = Board ID（本仓库 YH_JM 为 1）
Byte5 = 能力位：bit0 第二编码器，bit1 相电流采样，bit2 工厂固件，bit3 电流协议扩展
Byte6 = 固件主版本
Byte7 = 固件次版本
```

不认识该命令的旧固件回 `INVALID_COMMAND`。上位机按“能力未知”处理，保留原有 pu 运控；不启用需要确认量纲的电流扩展操作。

---

# 6. ENABLE

> **本固件：未实现。** 返回 `INVALID_COMMAND`。上电校准/加载成功后等待新实时命令恢复 PWM，无需 ENABLE。

请求：

```text
CAN ID = 0x200 + ID

Byte0 = 0x01
Byte1 = Sequence
Byte2~7 = 0
```

执行：

```text
检查故障
    ↓
启动驱动/PWM/FOC
    ↓
进入 READY
    ↓
发送 ACK
```

应答：

```text
0x280 + ID
```

---

# 7. DISABLE

> **本固件 1.3：** 已实现，但不进入 `DISABLED` 状态：关桥并清除保持目标后仍为 RUNNING，新实时命令即恢复。`CAN ID = 0x200` 广播时所有节点执行且不回 ACK。1.2 及更早返回 `INVALID_COMMAND`。

请求：

```text
Byte0 = 0x02
Byte1 = Sequence
```

执行：

```text
Torque Command = 0
       ↓
关闭 PWM
       ↓
State = DISABLED
       ↓
ACK
```

应答：

```text
0x280 + ID
```

---

# 8. SET_ZERO

> **本固件：** `RUNNING` 下可用。`FAULT` 时返回 `FAULT_ACTIVE`。把当前输出轴位置置零并写入 Flash；无有效 NV 记录时只改 RAM，返回 `INTERNAL_ERROR`。双编码器游标绝对角除以减速比 8，默认零点为游标原点（`CFG_MULTI_TURN_ZERO_RAD`，0），唯一区间为 31 圈电机轴；1.4 起上电取离零点最近的等价角度，`ABS_POSITION_UNKNOWN` 置位期间本命令返回 `INVALID_STATE`，见文首「位置警告」。

请求：

```text
Byte0 = 0x03
Byte1 = Sequence
```

作用：

```text
将当前输出轴连续多圈机械位置设置为：

Position = 0
```

应答：

```text
0x280 + ID
```

---

# 9. CLEAR_FAULT

> **本固件：** 运行时过流、母线失效、编码器和采样故障也会闩锁，恢复条件见文首。校准失败清除后仍须重新校准；其它运行时故障清除后等待新命令，不恢复旧指令。

请求：

```text
Byte0 = 0x04
Byte1 = Sequence
```

仅在实际故障条件已经消失时清除 Fault。

应答：

```text
0x280 + ID
```

如果故障条件仍存在：

```text
Result = FAULT_ACTIVE
```

---

# 10. START_ENCODER_CALIBRATION

> 固件 1.5 起，`0x05` 返回 `INVALID_COMMAND`，使用 [Rust 上位机校准试验扩展](../../docs/commissioning.md)。本节保留协议原文及固件 1.4 行为，供旧设备参考。
>
> **固件 1.4：**
> - 允许在 `RUNNING` 启动（原文仅 `DISABLED`）。`FAULT` → `FAULT_ACTIVE`。
> - ACK `OK` 且 State=`CALIBRATING` 后阻塞执行数秒；成功后 Motor State=`RUNNING`（原文为 `DISABLED`）。
> - 上电无 NV 时走同一流程并发 `0x3C0`，Sequence=0，没有本命令的 ACK。
> - 写 Flash 时短暂停 CAN。

启动编码器校准。

请求：

```text
CAN ID = 0x200 + MotorID

Byte0 = 0x05
Byte1 = Sequence
Byte2~7 = 0
```

校准仅允许在：

```text
Motor State = DISABLED
```

状态下启动。

收到命令后：

```text
检查当前状态
      ↓
检查基础硬件状态
      ↓
接受校准任务
      ↓
Motor State = CALIBRATING
      ↓
返回 ACK
      ↓
开始编码器校准
```

## 应答

首先返回：

```text
Command ACK
CAN ID = 0x280 + MotorID
```

ACK：

```text
Command = 0x05
Sequence = 请求中的 Sequence
Result = OK
State = CALIBRATING
```

这里的 `ACK = OK` 表示：

> 校准任务已经成功启动。

**不表示校准已经完成。**

校准完成状态由 `Encoder Calibration Report` 上报。

---

# 11. Encoder Calibration Report

编码器校准过程中，由电机主动上报。

```text
CAN ID = 0x3C0 + MotorID
Direction = Motor → Master
DLC = 8
```

## Payload

| Byte | 内容 |
|---|---|
| 0 | Sequence |
| 1 | Calibration State |
| 2 | Progress |
| 3 | Calibration Stage |
| 4~5 | Error Code `uint16` |
| 6~7 | Reserved |

### Sequence

与 `START_ENCODER_CALIBRATION` 请求中的 Sequence 一致。

用于主机确认当前进度属于哪一次校准任务。

---

## Calibration State

```text
0x00  IDLE
0x01  RUNNING
0x02  SUCCESS
0x03  FAILED
```

---

## Progress

```text
uint8
Range = 0 ~ 100

0   = 0%
25  = 25%
50  = 50%
100 = 100%
```

---

## Calibration Stage

```text
0x00  IDLE
0x01  PRE_CHECK
0x02  MOTOR_ALIGNMENT
0x03  ENCODER_SAMPLING
0x04  PARAMETER_CALCULATION
0x05  PARAMETER_SAVE
0x06  COMPLETE
```

---

## Error Code

```text
0x0000  NO_ERROR

0x0001  INVALID_STATE
0x0002  ENCODER_SIGNAL_ERROR
0x0003  CALIBRATION_TIMEOUT
0x0004  CALIBRATION_DATA_INVALID
0x0005  PARAMETER_SAVE_FAILED
0x0006  MOTOR_CONTROL_ERROR

0x00FF  INTERNAL_ERROR
```

> **本固件：** 实际会报的错误码：`0x0002` 编码器、`0x0004` 数据无效（无运动/极对数/探测速度过小）、`0x0005` 写 Flash 失败、`0x0006` 电机控制故障（校准中途跳闸或输出使能条件不满足）。其余错误码未使用。
>
> **校准动作（1.4）：** 开环正反各拖一整圈电机轴（电角速度 4π rad/s，14 对极时每趟 7 s），输出轴相应转过 45° 再转回，全程约 15 s；同一趟里测极对数、电角度零偏和游标零偏。1.3 及更早只拖 4 个电角圈，编码器非线性会让测得的极对数随起始位置变化（实测 13.8…14.6），可能报 `0x0004`。游标零偏不可用不算校准失败，只是不写零偏标志。

---

## 上报规则

校准过程中：

```text
State = RUNNING
```

电机周期主动发送：

```text
0x3C0 + ID
```

推荐周期：

```text
50 ms
```

即约：

```text
20 Hz
```

当 Stage 发生变化时立即额外发送一次。

> **固件 1.4** 的旧校准每 50 ms 上报，Stage 变化立即发送。固件 1.5 的试验状态由主机使用 `0x32` 查询。

---

## 校准成功

最终上报：

```text
Calibration State = SUCCESS
Progress          = 100
Calibration Stage = COMPLETE
Error Code        = 0x0000
```

随后：

```text
Motor State = DISABLED
```

电机需要重新收到 `ENABLE` 才允许进入运行状态。

> **本固件：** 成功后 Motor State=`RUNNING`，PWM 打开并保持当前位置，不需要 ENABLE。

---

## 校准失败

最终上报：

```text
Calibration State = FAILED
Progress          = 当前进度
Error Code        = 实际错误码
```

同时：

```text
Motor State = FAULT
Fault.CALIBRATION_FAULT = 1
```

之后需要：

```text
CLEAR_FAULT
      ↓
重新 START_ENCODER_CALIBRATION
```

---

# 12. GET_STATUS

> 固件 1.5 的校准会话使用 `0x32` 查询试验进度。常规状态字段的温度与警告定义见文首「温度与位置警告」。

请求：

```text
CAN ID = 0x200 + MotorID

Byte0 = 0x10
Byte1 = Sequence
Byte2~7 = 0
```

应答：

```text
CAN ID = 0x380 + MotorID
```

GET_STATUS **不再额外返回 Command ACK**。

`Status Response` 本身就是应答。

---

# 13. Status Response

```text
CAN ID = 0x380 + MotorID
Direction = Motor → Master
DLC = 8
```

## Payload

| Byte | 数据 |
|---|---|
| 0 | Motor State |
| 1~2 | Fault Code `uint16` |
| 3~4 | Bus Voltage `uint16` |
| 5~6 | Motor Temperature `int16` |
| 7 | Warning / Control Mode |

---

## Motor State

```text
0x00  DISABLED
0x01  READY
0x02  RUNNING
0x03  FAULT
0x04  CALIBRATING
```

> **本固件：** 不使用 `DISABLED`、`READY`。上电路径：校准/加载成功 → `RUNNING`；失败 → `FAULT`。CAN 校准：`RUNNING` → `CALIBRATING` → `RUNNING` 或 `FAULT`。`CLEAR_FAULT` 清校准故障后 State 显示 `RUNNING`，但此时尚未恢复运控输出。

状态转换：

```text
Power On
   ↓
DISABLED
   ↓ ENABLE
READY
   ↓ 与当前 Control Mode 匹配的有效实时控制命令
RUNNING
```

编码器校准：

```text
DISABLED
   ↓ START_ENCODER_CALIBRATION
CALIBRATING
   ↓ SUCCESS
DISABLED
```

> **本固件：** 成功路径为 `CALIBRATING` → `RUNNING`。

---

# 14. Fault Code

```text
uint16
```

Bit 定义：

```text
bit0   OVER_TEMPERATURE
bit1   OVER_CURRENT
bit2   OVER_VOLTAGE
bit3   UNDER_VOLTAGE

bit4   ENCODER_FAULT
bit5   STALL_OVERLOAD
bit6   DRIVER_FAULT
bit7   Reserved

bit8   POSITION_ERROR
bit9   MOTOR_PHASE_FAULT
bit10  CALIBRATION_FAULT

bit11~15 Reserved
```

多个故障允许同时置位。

> **本固件：** 已实现 bit1 过流、bit3 母线欠压/失效、bit4 编码器、bit10 校准、扩展 bit11 电流采样故障。其余未实现；判据及恢复见文首。

---

# 15. Bus Voltage

> **本固件：** `GET_STATUS` 填 PB0/ADC0_IN8 实测母线：`Vbus = raw/4095 × 3.3 × 31.303`（100 kΩ / 3.3 kΩ），再按 `0.01 V/LSB` 编码。主循环里软件触发。母线约 5 ms，固定在 ADC0 规则组；板温约 100 ms，单独用 ADC1。相电流在 ADC0 注入组，由 PWM 定时器在 20 kHz 中断里读取。运行中不关 ADC。

```text
uint16
0.01 V / LSB
```

换算：

```text
Voltage(V) = Raw × 0.01
```

---

# 16. Motor Temperature

> **本固件：未实现。** `GET_STATUS` 填 0。

```text
int16
0.1 °C / LSB
```

换算：

```text
Temperature(°C) = Raw × 0.1
```

---

# 17. Warning / Control Mode

> **本固件：** bit0 实现为原协议 `MOTOR_TEMPERATURE_WARNING`（需能力 bit6）。bit1…3 未实现填 0。bit4 在双编码器板上用作 `ABS_POSITION_UNKNOWN`（需能力 bit5；原协议该位叫 `POSITION_LIMIT_WARNING`，本固件未做限位）。`bit5~6` 回报 Control Mode；固件 1.2 起 `bit7` 回报 Inner Loop（需能力 bit3）。

```text
Byte7
```

原协议 Byte7 同时包含 Warning 和当前 Control Mode（本固件 bit7 的扩展见上方）：

```text
bit0  MOTOR_TEMPERATURE_WARNING
bit1  BUS_VOLTAGE_WARNING
bit2  CURRENT_LIMIT_ACTIVE
bit3  TORQUE_LIMIT_ACTIVE
bit4  POSITION_LIMIT_WARNING

bit5~6 CONTROL_MODE
bit7   Reserved
```

Control Mode 解码：

```text
ControlMode = (Byte7 >> 5) & 0x03

0x00  MOTION_MODE
0x01  VELOCITY_MODE
0x02  POSITION_MODE
0x03  Reserved
```

Warning 使用 `Byte7 & 0x1F` 提取，且不要求电机进入 Fault。主机可以通过 GET_STATUS 检查 `SET_CONTROL_MODE` 后的当前模式。

---

# 18. Command ACK

```text
CAN ID = 0x280 + MotorID
Direction = Motor → Master
DLC = 8
```

格式：

| Byte | 内容 |
|---|---|
| 0 | Command |
| 1 | Sequence |
| 2 | Result |
| 3 | Motor State |
| 4~5 | Fault Code |
| 6~7 | Reserved |

Command：

```text
0x01 ENABLE
0x02 DISABLE
0x03 SET_ZERO
0x04 CLEAR_FAULT
0x05 START_ENCODER_CALIBRATION
0x06 SET_CONTROL_MODE
0x07 SET_NODE_ID（本固件扩展）
0x08 SAVE_USER_PARAMS（本固件扩展）
0x20 SET_GAINS
0x21 GET_GAINS（本固件扩展）
```

Result：

```text
0x00  OK
0x01  INVALID_COMMAND
0x02  INVALID_STATE
0x03  PARAMETER_OUT_OF_RANGE
0x04  FAULT_ACTIVE
0x05  BUSY
0x06  INTERNAL_ERROR
```

> **本固件：** `ENABLE` 及未知 Command → `INVALID_COMMAND`（1.2 及更早的 `DISABLE` 同样）。`BUSY` 用于 `CALIBRATING` 时的第二条启动/SAVE。`INTERNAL_ERROR` 用于 `SET_NODE_ID` / `SAVE_USER_PARAMS` 写 Flash 失败。

---

# 19. 应答规则

| 主机发送 | 电机应答 |
|---|---|
| `Motion Command 0x100` | 当前模式为 MOTION 时返回 **`Motion Feedback 0x300`** |
| `Velocity Mode Command 0x140` | 当前模式为 VELOCITY 时返回 **`Motion Feedback 0x300`** |
| `Control Gains 0x180` | **`Command ACK 0x280`** |
| `GET_GAINS` | 成功：**`0x180` Gains 布局**（无额外 ACK）；失败：`Command ACK 0x280` |
| `SAVE_USER_PARAMS` | **`Command ACK 0x280`** |
| `Position Mode Command 0x1C0` | 当前模式为 POSITION 时返回 **`Motion Feedback 0x300`** |
| `ENABLE` | **`Command ACK 0x280`** |
| `DISABLE` | **`Command ACK 0x280`** |
| `SET_ZERO` | **`Command ACK 0x280`** |
| `CLEAR_FAULT` | **`Command ACK 0x280`** |
| `START_ENCODER_CALIBRATION` | 固件 1.4 先 `ACK 0x280`，再发 `Calibration Report 0x3C0`。固件 1.5 使用校准试验扩展。 |
| `SET_CONTROL_MODE` | **`Command ACK 0x280`** |
| `SET_NODE_ID`（本固件） | **`Command ACK 0x280`（旧 ID），随后复位** |
| `GET_STATUS` | **`Status Response 0x380`** |

> **本固件：** `0x100`/`0x140`/`0x1C0` 均按当前模式回 Feedback。`ENABLE` 仍回 `0x280`，Result=`INVALID_COMMAND`；1.3 的 `DISABLE` 回 `OK`，`GET_FEEDBACK` 回 `0x300`。

表中的实时控制命令应答表示正常的单帧处理结果；如果一次接收处理中积累了多帧实时控制命令，不要求逐帧返回 Motion Feedback，按照下述合并规则处理。

电机发送以下帧后，主机不需要再次 ACK：

```text
0x280 Command ACK
0x300 Motion Feedback
0x380 Status Response
0x3C0 Encoder Calibration Report
```

## 19.1 实时控制帧合并规则

一次接收处理是指：驱动器进入 CAN 接收任务后，依次取出当时 RX FIFO 中的待处理帧，直到 FIFO 被取空。

对于以下三种实时控制命令：

```text
0x100 + MotorID  Motion Command
0x140 + MotorID  Velocity Mode Command
0x1C0 + MotorID  Position Mode Command
```

如果一次接收处理中存在多帧，驱动器必须读取并校验所有帧，但只保留最后收到的一帧与当前 Control Mode 匹配的有效实时控制命令：

```text
读取并校验 RX FIFO 中的所有帧
              ↓
管理/参数命令按接收顺序逐帧处理并应答
              ↓
保留最后一帧与当前 Control Mode 匹配的有效命令
              ↓
执行最新有效实时控制命令一次
              ↓
返回一帧最新 Motion Feedback
```

具体规则：

- “最新”以驱动器从 RX FIFO 取出帧的顺序为准。
- 实时控制帧不能隐式切换 Control Mode。与当前模式不匹配的实时帧视为无效帧，不执行且不反馈。
- 无效帧、模式不匹配帧、DLC 错误帧和参数越界帧不能覆盖此前已经收到的有效实时控制命令。
- 驱动器必须取出并检查所有待处理帧，不能因为只执行最新命令而只读取 RX FIFO 的最后一帧。
- 本次没有有效实时控制命令时，不执行新的控制量，也不因这些无效帧发送 Motion Feedback。
- Motion Feedback 表示执行最新控制命令后的电机当前状态，不是对每一帧实时控制命令的独立确认。

当实时控制命令与 Control Gains、Management Command 同时积压时，驱动器先按接收顺序处理参数和管理命令，再根据处理后的 Motor State 和 Control Mode 校验并执行最新实时控制命令。这使 DISABLE、SET_CONTROL_MODE、故障处理等管理动作优先于实时控制输出。

> **本固件：** 合并规则作用于与当前 Control Mode 匹配的 Motion / Velocity / Position 帧。`SET_CONTROL_MODE` 会丢弃本轮中切换前已解析的实时帧。`START_ENCODER_CALIBRATION` 结束后会清空 RX FIFO 并丢弃本次已解析的实时帧。

## 19.2 非实时命令处理规则

以下命令不能合并为最新一帧，驱动器必须按接收顺序逐帧处理并给出对应应答：

| 命令类型 | 处理方式 |
|---|---|
| Control Gains | 每帧处理并返回 Command ACK |
| ENABLE / DISABLE | 每帧处理并返回 Command ACK |
| SET_ZERO / CLEAR_FAULT | 每帧处理并返回 Command ACK |
| START_ENCODER_CALIBRATION | 每帧处理并返回 Command ACK；接受后再主动上报进度 |
| SET_CONTROL_MODE | 每帧处理并返回 Command ACK |
| SET_NODE_ID | 每帧处理并返回 Command ACK；成功则随后复位 |
| GET_STATUS | 每帧返回 Status Response |

新的非实时请求使用新的 Sequence。主机因应答超时重发同一请求时，必须使用与原请求相同的 Command 和 Sequence。推荐驱动器缓存最近一次非实时请求及其应答；收到相同 Command 和 Sequence 的重复请求时，重新发送缓存的应答，不重复执行命令。

## 19.3 上位机请求建议

### 实时控制请求

- 对同一电机只周期发送当前所选模式的一种实时控制命令，不要在一个控制周期内同时发送 Motion、Velocity 和 Position 命令。
- 使用满足控制性能和总线负载要求的稳定周期发送，不要集中突发发送多帧；常用建议值为 10~20 ms。
- 目标值更新速度高于 CAN 发送速度时，上位机只发送当前发送时刻的最新目标值，不应把已经过时的目标值排队补发。
- 不要求发送一帧后等待对应 Motion Feedback 才能发送下一帧。上位机应把 Motion Feedback 作为最新状态流处理，并记录接收时间用于判断反馈超时。
- 切换实时控制模式时，停止发送旧模式帧并清空上位机的旧实时帧发送队列，然后按照 `DISABLE → SET_CONTROL_MODE → ENABLE` 的顺序操作。收到各步骤 ACK 后，再开始周期发送新模式的实时控制帧。
- 通信停止不会自动清零控制输出或进入 Fault，因此上位机正常停止控制时必须发送 DISABLE，并监控反馈超时作为通信异常提示。
- 多电机系统应错开各电机请求的发送相位，避免所有电机在同一时刻集中发送命令和反馈。

> **本固件：** 1.3 起停止周期实时命令后应发 `DISABLE`（或 `0x200` 广播），否则电机仍保持最后一帧目标；1.2 及更早无 `DISABLE`。模式切换无需 `DISABLE → SET_CONTROL_MODE → ENABLE`：先停旧模式循环，发 `SET_CONTROL_MODE`，再发新模式 Gains（若未配置）和实时命令。校准前应先停止实时循环。

### 非实时请求

- 对同一电机，推荐同一时刻只保留一个等待应答的非实时请求；收到对应 Sequence 的应答或确认超时后，再发送下一条请求。
- 设置 Control Gains 时，在 Byte0 分别指定 `MOTION_MODE`、`VELOCITY_MODE` 或 `POSITION_MODE`，每次收到对应 ACK 后再设置下一组参数。进入速度模式前应确认速度 PI 已配置；进入位置模式前应确认速度 PI 和位置 PID 均已配置。
- 每个新请求的 Sequence 加 1，`0xFF` 后回绕到 `0x00`；重试同一请求时保持原 Sequence 不变。
- 推荐 ACK 和 Status Response 等待超时为 20 ms；超时后最多重试 2 次。具体值可以根据固件任务周期和总线负载调整。
- 收到 Command ACK 时，同时校验 CAN ID、Command 和 Sequence，不能只根据 Sequence 判断请求是否完成。
- SET_ZERO、ENABLE、SET_CONTROL_MODE、START_ENCODER_CALIBRATION 等具有状态影响的命令，在未收到应答前不要改用新的 Sequence 重发，否则驱动器可能将其视为新的操作。
- START_ENCODER_CALIBRATION 收到 `ACK = OK` 后进入进度等待阶段，不要继续重发启动命令；完成状态以 Encoder Calibration Report 为准。
- 管理操作期间避免与实时控制请求混发。推荐先停止实时请求，完成管理命令及应答，再恢复对应模式的周期控制请求。

---

# 20. 最终 CAN ID 表

```text
0x100 + ID
Motion Command
Master → Motor

0x140 + ID
Velocity Mode Command
Master → Motor

0x180 + ID
Control Gains
Master → Motor

0x1C0 + ID
Position Mode Command
Master → Motor

0x200 + ID
Management Command
Master → Motor

0x280 + ID
Command ACK
Motor → Master

0x300 + ID
Motion Feedback
Motor → Master

0x380 + ID
Status Response / Fault Event
Motor → Master

0x3C0 + ID
Encoder Calibration Report
Motor → Master
```

> **本固件：** `0x380` 作为 `GET_STATUS` 的 Status Response；1.3 起新故障位、1.4 起新警告位出现时主动发送一帧，格式相同。`0x200` 为广播 DISABLE。

## 推荐通信流程

```text
上电
 │
 ▼
DISABLED
 │
 ├──── Set Gains by Mode ─────►
 │◄──── ACK ───────────────────
 │
 ├──── Encoder Calibration ───►
 │◄──── ACK(Start Accepted) ───
 │
 │◄──── Calibration 5% ────────
 │◄──── Calibration 20% ───────
 │◄──── Calibration 50% ───────
 │◄──── Calibration 80% ───────
 │◄──── Calibration 100% ──────
 │
 ├──── Set Control Mode ──────►
 │◄──── ACK ───────────────────
 │
 ├──── Enable ────────────────►
 │◄──── ACK ───────────────────
 │
 ▼
READY
 │
 ├──── 与当前模式匹配的 ──────►
 │     实时控制命令
 │◄──── Motion Feedback ───────
 │
 ├──── 同类型实时控制命令 ────►
 │◄──── Motion Feedback ───────
 │
 ▼
RUNNING
```

> **本固件实际上电流程（不要按上方原文发 ENABLE/DISABLE）：**
>
> ```text
> 上电（CAN 已启动）
>  │
>  ├─ 有有效 NV ──► 加载参数 ──► RUNNING（无 0x3C0）
>  │
>  └─ 无 NV ──► 与 START_CALI 同一套校准
>        │◄──── 0x3C0 Sequence=0 进度
>        ├─ SUCCESS ──► 写 Flash ──► RUNNING
>        └─ FAILED  ──► FAULT（CALIBRATION_FAULT）
>              ├──── CLEAR_FAULT ─────────► ACK
>              └──── START_CALI ──────────► ACK(CALIBRATING) + 0x3C0
>                    SUCCESS ──► RUNNING
>  │
>  ├──── SET_GAINS(MOTION) ────► ACK
>  ├──── Motion Command ───────► Motion Feedback
>  ▼
> RUNNING
> ```
>
> 校准前先停止实时循环。速度模式先配 VELOCITY Gains 再 `SET_CONTROL_MODE`；位置模式还需再配 POSITION Gains。
