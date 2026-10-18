# Motor CAN Protocol V1.0

[中文](Motor%20CAN%20Protocol%20V1.0.md) | [English](Motor%20CAN%20Protocol%20V1.0.en.md)

Protocol actually implemented by current YHorizon-JM firmware. Classic CAN 2.0, 11-bit ID, DLC = 8, 1 Mbps, little-endian. Motor ID = 1~63. Position and velocity are on the **output shaft** (motor shaft / gear ratio 8).

After power-on calibration or a successful Flash load the drive enters `RUNNING`. There is no `ENABLE` / `DISABLE`. Current inner loop by default. If the motor parameters differ and the current loop will not run, set `CFG_INNER_LOOP` in `config.h` to `CFG_INNER_LOOP_VOLTAGE` to switch back to the voltage inner loop.

The frame layout is identical for both inner loops. `t_ref`, the feedforward and the gains all use one per-unit torque command, clamped to `±V_LIMIT`. The firmware interprets it by inner loop:

| Inner loop | Meaning of `t_ref` | Conversion |
| --- | --- | --- |
| Voltage | `Vq`, relative to bus voltage | `Vq ≈ t_ref × Vbus` (V) |
| Current | `Iq` command | `Iq_ref = t_ref × I_LIMIT / V_LIMIT`, default `10 A/pu` |

The active inner loop is reported in `GET_STATUS` Byte7 bit7. Outer-loop gains mean something different under the current loop; retune them when switching from the voltage loop.

## CAN IDs

| CAN ID | Direction | Function |
| --- | --- | --- |
| `0x100 + ID` | Host → motor | Motion command |
| `0x140 + ID` | Host → motor | Velocity command |
| `0x180 + ID` | Host → motor | SET gains |
| `0x180 + ID` | Motor → host | GET gains reply |
| `0x1C0 + ID` | Host → motor | Position command |
| `0x200 + ID` | Host → motor | Management |
| `0x280 + ID` | Motor → host | Command ACK |
| `0x300 + ID` | Motor → host | Motion feedback |
| `0x380 + ID` | Motor → host | GET_STATUS reply |
| `0x3C0 + ID` | Motor → host | Calibration progress |

## Control mode and state

```text
Control Mode    0x00 MOTION    0x01 VELOCITY    0x02 POSITION
Motor State     0x02 RUNNING   0x03 FAULT       0x04 CALIBRATING
```

Power-on default is `MOTION`. `SET_CONTROL_MODE` may switch while `RUNNING`; integrators are cleared and `t_ref` is 0 until a valid real-time command for the new mode arrives. The drive executes only real-time frames that match the current mode; others are dropped with no feedback.

If several real-time frames arrive in one RX pass, only the last valid one is applied. There is no timeout: the last frame is held until a new command of the same mode, a mode change, calibration, FAULT, or power-off. To stop, send zero speed / current position, or power down.

Calibration blocks the main loop for several seconds. Other commands are not handled during that time; queued motion frames are discarded afterward.

## Real-time commands

Call `SET_CONTROL_MODE` first, then send matching real-time frames. A valid frame is answered with `0x300` and no extra ACK.

### Motion `0x100 + ID`

| Byte | Field | Type | Encoding |
| --- | --- | --- | --- |
| 0~3 | Position command | `int32` | `0.0001 rad/LSB` |
| 4~5 | Velocity command | `int16` | `0.1 rad/s/LSB`; values beyond about `±3276.7` are clamped, then still executed |
| 6~7 | Torque FF | `int16` | `raw / 32767` → −1~1 pu |

```text
t_ref = Kd*(v_set - v_act) + Kp*(p_set - p_act) + t_ff
t_ref = clamp(t_ref, ±V_LIMIT)
```

`V_LIMIT` defaults to 0.50 pu. Under the voltage loop 1.0 pu = full PWM; under the current loop 0.50 pu corresponds to `I_LIMIT` = 5 A.

### Velocity `0x140 + ID`

| Byte | Field | Type | Encoding |
| --- | --- | --- | --- |
| 0~3 | Velocity command | `int32` | `0.1 rad/s/LSB` |
| 4~7 | Reserved | | Must be 0, otherwise the frame is dropped |

```text
u = Kp*e_v + Ki*∫e_v dt     integral clamped to ±V_LIMIT
```

### Position `0x1C0 + ID`

| Byte | Field | Type | Encoding |
| --- | --- | --- | --- |
| 0~3 | Position command | `int32` | `0.0001 rad/LSB` |
| 4~7 | Maximum velocity | `uint32` | `0.1 rad/s/LSB`, absolute limit |

A position PID outer loop builds `v_ref` (clamped to `±vmax`), then the velocity PI inner loop runs with the VELOCITY gain set. Outer-loop integration freezes while the inner loop is saturated.

### Motion feedback `0x300 + ID`

| Byte | Field | Type | Encoding |
| --- | --- | --- | --- |
| 0~3 | Position | `int32` | `0.0001 rad/LSB`, continuous multi-turn |
| 4~5 | Velocity | `int16` | `0.1 rad/s/LSB` |
| 6~7 | Torque command `t_ref` | `int16` | `0.001 pu/LSB` (command, not measured torque; convert to V or A by inner loop) |

Reported only after a valid real-time command.

## Gains `0x180 + ID`

SET: host → motor. Successful GET: motor → host, same layout.

| Byte | Field |
| --- | --- |
| 0 | Control Mode |
| 1 | Sequence |
| 2~3 | Kp `uint16` |
| 4~5 | Ki `uint16` |
| 6~7 | Kd `uint16` |

| Mode | Kp | Ki | Kd |
| --- | --- | --- | --- |
| MOTION | `0.01 pu/rad` | Must be 0 | `0.001 pu·s/rad` |
| VELOCITY | `0.001 pu/(rad/s)` | `0.001 pu/rad` | Must be 0 |
| POSITION | `0.01 s⁻¹` | `0.001 s⁻²` | `0.001` |

SET updates RAM only. ACK is on `0x280` (Command=`0x20`). Illegal mode, or a gain that must be 0 is not 0 → `PARAMETER_OUT_OF_RANGE`, previous parameters kept. Motion gains are not stored across power cycles; velocity/position gains are written to Flash with `SAVE_USER_PARAMS`.

GET_GAINS (`0x200`, Byte0=`0x21`, Byte2=mode, rest 0): success returns current RAM on `0x180` with no extra ACK; failure returns an error on `0x280`.

## Management `0x200 + ID`

| Byte | Field |
| --- | --- |
| 0 | Command |
| 1 | Sequence |
| 2~7 | Parameters; unused bytes must be 0 |

| Command | Value | Notes |
| --- | --- | --- |
| SET_ZERO | `0x03` | Zero the current output shaft and write Flash |
| CLEAR_FAULT | `0x04` | Clears `CALIBRATION_FAULT` only. Afterward State=`RUNNING` but PWM is off; calibrate again before motion |
| START_ENCODER_CALIBRATION | `0x05` | Allowed in `RUNNING`; ACK first, then calibrate |
| SET_CONTROL_MODE | `0x06` | Byte2 = mode. `FAULT` → `INVALID_STATE` |
| SET_NODE_ID | `0x07` | Byte2 = new ID (1~63). ACK on the old ID (Byte6=new ID), then reset after about 20 ms |
| SAVE_USER_PARAMS | `0x08` | Write velocity Kp/Ki and position Kp/Ki/Kd to the same Flash page as calibration |
| GET_STATUS | `0x10` | Reply on `0x380`, no extra ACK |
| GET_GAINS | `0x21` | See previous section |

Unknown commands (including `ENABLE 0x01` / `DISABLE 0x02`) return `INVALID_COMMAND`. A repeat of the same Command+Sequence returns the cached reply and is not executed again.

### Command ACK `0x280 + ID`

| Byte | Field |
| --- | --- |
| 0 | Command |
| 1 | Sequence |
| 2 | Result |
| 3 | Motor State |
| 4~5 | Fault Code |
| 6 | New ID after a successful SET_NODE_ID, otherwise 0 |
| 7 | 0 |

```text
Result  0x00 OK
        0x01 INVALID_COMMAND
        0x02 INVALID_STATE
        0x03 PARAMETER_OUT_OF_RANGE
        0x04 FAULT_ACTIVE
        0x05 BUSY            // start/SAVE while calibrating
        0x06 INTERNAL_ERROR  // Flash write failed
```

### GET_STATUS reply `0x380 + ID`

| Byte | Field |
| --- | --- |
| 0 | Motor State |
| 1~2 | Fault Code `uint16` |
| 3~4 | Bus voltage `uint16`, `0.01 V/LSB` (measured on PB0) |
| 5~6 | Temperature, currently 0 |
| 7 | bits 5~6 = Control Mode; bit 7 = inner loop (0 voltage, 1 current); warning bits 0~4 are 0 |

Only fault bit `bit10 CALIBRATION_FAULT` is used. No unsolicited Fault Event; status is returned only for `GET_STATUS`.

## Encoder calibration `0x3C0 + ID`

`START_ENCODER_CALIBRATION` ACKs first (State=`CALIBRATING`), then runs blocked. A report is sent about every 50 ms, plus immediately on Stage change. Success returns to `RUNNING`; failure goes to `FAULT` + `CALIBRATION_FAULT`. Power-on with invalid NV uses the same path and reports with Sequence=0 and no Command ACK.

| Byte | Field |
| --- | --- |
| 0 | Sequence |
| 1 | State: `0x01 RUNNING` / `0x02 SUCCESS` / `0x03 FAILED` |
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

CAN is paused briefly while writing Flash, so reports may skip during `PARAMETER_SAVE`.

## Suggested sequence

```text
Power on → wait until calibration finishes or GET_STATUS is RUNNING
SET_CONTROL_MODE
SET_GAINS (for that mode)
Stream matching real-time commands, receive 0x300
SAVE_USER_PARAMS / SET_ZERO / SET_NODE_ID as needed
```

ID can also be changed with the button: hold PB1 for 2 s to enter, short press to increment, hold again to save and reset. The LED blinks the ID (tens as long blinks, ones as short blinks).

## Later work

This is a single-axis Classic CAN 1 Mbps protocol. Planned next:

- **CAN FD** after moving the MCU to **GD32C113**
- Multi-axis sync (shared clock / sync frames, batched position or impedance)
- `ENABLE` / `DISABLE` and fuller fault detection (over-current, over-voltage, over-temperature, stall)
- Measured torque (Nm) feedback and unsolicited Fault Events
