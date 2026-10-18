# Motor CAN Protocol V1.0

[中文](Motor%20CAN%20Protocol%20V1.0.md) | [English](Motor%20CAN%20Protocol%20V1.0.en.md)

YHorizon-JM 当前固件实际使用的协议。Classic CAN 2.0，11-bit ID，DLC = 8，1 Mbps，小端。Motor ID = 1~63。位置、速度均指**输出轴**（电机轴 / 减速比 8）。

上电校准或加载 Flash 成功后直接进入 `RUNNING`，没有 `ENABLE` / `DISABLE`。默认电流内环；电机参数不一致、电流环跑不通时，把 `config.h` 的 `CFG_INNER_LOOP` 改为 `CFG_INNER_LOOP_VOLTAGE` 切回电压内环。

两种内环下帧格式完全一样，`t_ref`、前馈和增益都用同一个力矩指令标幺值 pu，限幅 `±V_LIMIT`。固件按内环解释它：

| 内环 | `t_ref` 的含义 | 换算 |
| --- | --- | --- |
| 电压 | `Vq`，相对母线电压 | `Vq ≈ t_ref × Vbus`（V） |
| 电流 | `Iq` 指令 | `Iq_ref = t_ref × I_LIMIT / V_LIMIT`，默认 `10 A/pu` |

当前内环在 `GET_STATUS` 的 Byte7 bit7 上报。电流内环下外环增益的物理含义变了，从电压环切过来要重新整定。

## CAN ID

| CAN ID | 方向 | 功能 |
| --- | --- | --- |
| `0x100 + ID` | 主机 → 电机 | Motion Command |
| `0x140 + ID` | 主机 → 电机 | Velocity Command |
| `0x180 + ID` | 主机 → 电机 | SET Gains |
| `0x180 + ID` | 电机 → 主机 | GET Gains 应答 |
| `0x1C0 + ID` | 主机 → 电机 | Position Command |
| `0x200 + ID` | 主机 → 电机 | 管理命令 |
| `0x280 + ID` | 电机 → 主机 | Command ACK |
| `0x300 + ID` | 电机 → 主机 | Motion Feedback |
| `0x380 + ID` | 电机 → 主机 | GET_STATUS 应答 |
| `0x3C0 + ID` | 电机 → 主机 | 校准进度 |

## 控制模式与状态

```text
Control Mode    0x00 MOTION    0x01 VELOCITY    0x02 POSITION
Motor State     0x02 RUNNING   0x03 FAULT       0x04 CALIBRATING
```

上电默认 `MOTION`。`SET_CONTROL_MODE` 在 `RUNNING` 下切换；切换后清积分，`t_ref` 为 0，直到收到新模式的有效实时命令。驱动器只执行与当前模式匹配的实时帧，其它实时帧丢弃且不回 Feedback。

同一轮 RX 若有多帧实时命令，只执行最后一帧有效命令。无超时：保持最后一帧，直到同模式新命令、切模式、校准、FAULT 或掉电。停止运动请发零速度/当前位置，或断电。

校准会阻塞主循环数秒；期间不处理其它命令，结束后丢弃积压的运控帧。

## 实时命令

先 `SET_CONTROL_MODE`，再发对应实时帧。有效帧后回 `0x300`，无额外 ACK。

### Motion `0x100 + ID`

| Byte | 数据 | 类型 | 编码 |
| --- | --- | --- | --- |
| 0~3 | 目标位置 | `int32` | `0.0001 rad/LSB` |
| 4~5 | 目标速度 | `int16` | `0.1 rad/s/LSB`，超出约 `±3276.7` 则钳位后仍执行 |
| 6~7 | 力矩前馈 | `int16` | `raw / 32767` → −1~1 pu |

```text
t_ref = Kd*(v_set - v_act) + Kp*(p_set - p_act) + t_ff
t_ref = clamp(t_ref, ±V_LIMIT)
```

`V_LIMIT` 默认 0.50 pu。电压内环下 1.0 pu = PWM 满幅；电流内环下 0.50 pu 对应 `I_LIMIT` = 5 A。

### Velocity `0x140 + ID`

| Byte | 数据 | 类型 | 编码 |
| --- | --- | --- | --- |
| 0~3 | 目标速度 | `int32` | `0.1 rad/s/LSB` |
| 4~7 | Reserved | | 必须为 0，否则丢弃 |

```text
u = Kp*e_v + Ki*∫e_v dt     积分钳位到 ±V_LIMIT
```

### Position `0x1C0 + ID`

| Byte | 数据 | 类型 | 编码 |
| --- | --- | --- | --- |
| 0~3 | 目标位置 | `int32` | `0.0001 rad/LSB` |
| 4~7 | 最大速度 | `uint32` | `0.1 rad/s/LSB`，绝对值上限 |

位置 PID 外环生成 `v_ref`（限幅 `±vmax`），再走速度 PI 内环（使用 VELOCITY 那组增益）。内环饱和时冻结外环积分。

### Motion Feedback `0x300 + ID`

| Byte | 数据 | 类型 | 编码 |
| --- | --- | --- | --- |
| 0~3 | 当前位置 | `int32` | `0.0001 rad/LSB`，连续多圈 |
| 4~5 | 当前速度 | `int16` | `0.1 rad/s/LSB` |
| 6~7 | 力矩指令 `t_ref` | `int16` | `0.001 pu/LSB`（指令值，不是实测转矩；按内环换算成 V 或 A） |

只在有效实时命令之后上报。

## 增益 `0x180 + ID`

SET：主机 → 电机。GET 成功：电机 → 主机，布局相同。

| Byte | 内容 |
| --- | --- |
| 0 | Control Mode |
| 1 | Sequence |
| 2~3 | Kp `uint16` |
| 4~5 | Ki `uint16` |
| 6~7 | Kd `uint16` |

| 模式 | Kp | Ki | Kd |
| --- | --- | --- | --- |
| MOTION | `0.01 pu/rad` | 必须为 0 | `0.001 pu·s/rad` |
| VELOCITY | `0.001 pu/(rad/s)` | `0.001 pu/rad` | 必须为 0 |
| POSITION | `0.01 s⁻¹` | `0.001 s⁻²` | `0.001` |

SET 只改 RAM，ACK 在 `0x280`（Command=`0x20`）。Mode 非法或应为 0 的增益非 0 → `PARAMETER_OUT_OF_RANGE`，原参数不变。Motion 增益不上电保存；速度/位置增益用 `SAVE_USER_PARAMS` 写入 Flash。

GET_GAINS（`0x200`，Byte0=`0x21`，Byte2=mode，其余 0）：成功在 `0x180` 回当前 RAM，无额外 ACK；失败在 `0x280` 回错误。

## 管理命令 `0x200 + ID`

| Byte | 内容 |
| --- | --- |
| 0 | Command |
| 1 | Sequence |
| 2~7 | 参数，未用字节必须为 0 |

| Command | 值 | 说明 |
| --- | --- | --- |
| SET_ZERO | `0x03` | 当前输出轴置零并写 Flash |
| CLEAR_FAULT | `0x04` | 仅清除 `CALIBRATION_FAULT`。清除后 State=`RUNNING`，PWM 关闭，需再校准才接受运控 |
| START_ENCODER_CALIBRATION | `0x05` | `RUNNING` 下可启动；先 ACK 再校准 |
| SET_CONTROL_MODE | `0x06` | Byte2 = 模式。`FAULT` → `INVALID_STATE` |
| SET_NODE_ID | `0x07` | Byte2 = 新 ID（1~63）。旧 ID 回 ACK（Byte6=新 ID）后约 20 ms 复位 |
| SAVE_USER_PARAMS | `0x08` | 把速度 Kp/Ki 与位置 Kp/Ki/Kd 写入校准同一 Flash 页 |
| GET_STATUS | `0x10` | 应答在 `0x380`，无额外 ACK |
| GET_GAINS | `0x21` | 见上一节 |

未知命令（含 `ENABLE 0x01` / `DISABLE 0x02`）回 `INVALID_COMMAND`。相同 Command+Sequence 重发返回缓存应答，不重复执行。

### Command ACK `0x280 + ID`

| Byte | 内容 |
| --- | --- |
| 0 | Command |
| 1 | Sequence |
| 2 | Result |
| 3 | Motor State |
| 4~5 | Fault Code |
| 6 | SET_NODE_ID 成功时为新 ID，其它为 0 |
| 7 | 0 |

```text
Result  0x00 OK
        0x01 INVALID_COMMAND
        0x02 INVALID_STATE
        0x03 PARAMETER_OUT_OF_RANGE
        0x04 FAULT_ACTIVE
        0x05 BUSY          // 校准中再发启动/SAVE
        0x06 INTERNAL_ERROR  // 写 Flash 失败
```

### GET_STATUS 应答 `0x380 + ID`

| Byte | 内容 |
| --- | --- |
| 0 | Motor State |
| 1~2 | Fault Code `uint16` |
| 3~4 | 母线电压 `uint16`，`0.01 V/LSB`（PB0 实测） |
| 5~6 | 温度，当前填 0 |
| 7 | bit5~6 = Control Mode；bit7 = 内环（0 电压，1 电流）；Warning 位 bit0~4 填 0 |

当前仅使用故障位 `bit10 CALIBRATION_FAULT`。不主动发 Fault Event，只响应 `GET_STATUS`。

## 编码器校准 `0x3C0 + ID`

`START_ENCODER_CALIBRATION` 先 ACK（State=`CALIBRATING`），再阻塞执行。约 50 ms 上报一帧，Stage 变化立即多发一帧。成功后回 `RUNNING`；失败进入 `FAULT` + `CALIBRATION_FAULT`。上电无有效 NV 时走同一套流程并上报，Sequence=0，没有 Command ACK。

| Byte | 内容 |
| --- | --- |
| 0 | Sequence |
| 1 | State：`0x01 RUNNING` / `0x02 SUCCESS` / `0x03 FAILED` |
| 2 | Progress 0~100 |
| 3 | Stage |
| 4~5 | Error Code |
| 6~7 | 0 |

```text
Stage   0x01 PRE_CHECK
        0x02 MOTOR_ALIGNMENT
        0x03 ENCODER_SAMPLING
        0x04 PARAMETER_CALCULATION
        0x05 PARAMETER_SAVE
        0x06 COMPLETE

Error   0x0000 NO_ERROR
        0x0002 ENCODER_SIGNAL_ERROR
        0x0004 CALIBRATION_DATA_INVALID
        0x0005 PARAMETER_SAVE_FAILED
```

写 Flash 时短暂停 CAN，`PARAMETER_SAVE` 阶段上报可能有缺口。

## 推荐流程

```text
上电 → 等校准结束或 GET_STATUS 为 RUNNING
SET_CONTROL_MODE
SET_GAINS（按模式）
周期发送对应实时命令，收 0x300
需要时 SAVE_USER_PARAMS / SET_ZERO / SET_NODE_ID
```

也可用按键改 ID：PB1 长按 2 s 进入，短按 +1，再长按保存并复位。LED 按 ID 闪烁（十位长闪 + 个位短闪）。

## 后续计划

当前为 Classic CAN 1 Mbps 的单轴协议。后续准备：

- 主控换到 **GD32C113** 后支持 **CAN FD**
- 多轴同步（共享时钟/同步帧，批量下发位置或阻抗）
- `ENABLE` / `DISABLE` 与更完整的故障检测（过流、过压、过温、堵转）
- 实测转矩（Nm）反馈与主动 Fault Event
