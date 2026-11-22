//! Motor CAN Protocol V1.0 打包与解包。
//!
//! Classic CAN，标准帧 8 字节，小端。节点号 1..=63，仲裁 ID = 基址 + 节点号。

/// 位置 LSB，rad。
pub const POS_LSB: f64 = 0.0001;
/// 速度 LSB，rad/s。
pub const VEL_LSB: f64 = 0.1;
/// 运控 Kp LSB，输出轴 Nm/rad。
pub const KP_LSB_MOTION: f64 = 0.01;
/// 运控 Kd LSB，输出轴 Nm·s/rad。
pub const KD_LSB_MOTION: f64 = 0.001;
/// 速度环 Kp LSB。
pub const KP_LSB_VEL: f64 = 0.001;
/// 速度环 Ki LSB。
pub const KI_LSB_VEL: f64 = 0.001;
/// 位置环 Kp LSB。
pub const KP_LSB_POS: f64 = 0.01;
/// 位置环 Ki LSB。
pub const KI_LSB_POS: f64 = 0.001;
/// 位置环 Kd LSB。
pub const KD_LSB_POS: f64 = 0.001;
/// 运控力矩 LSB，输出轴 Nm。前馈和反馈共用。
pub const TORQUE_LSB: f64 = 0.001;
/// 母线电压 LSB，V。
pub const VBUS_LSB: f64 = 0.01;
/// 温度 LSB，°C。
pub const TEMP_LSB: f64 = 0.1;

/// 电压内环。
pub const INNER_LOOP_VOLTAGE: u8 = 0;
/// 电流内环。
pub const INNER_LOOP_CURRENT: u8 = 1;

/// Motion 模式。
pub const MODE_MOTION: u8 = 0x00;
/// 速度模式。
pub const MODE_VELOCITY: u8 = 0x01;
/// 位置模式。
pub const MODE_POSITION: u8 = 0x02;

/// 管理命令：使能。本固件返回 INVALID_COMMAND。
pub const CMD_ENABLE: u8 = 0x01;
/** @brief 管理命令：失能。1.3 起关桥并清除保持的实时目标；节点号 0 广播时不回 ACK。 */
pub const CMD_DISABLE: u8 = 0x02;
/// 当前位置写零点。
pub const CMD_SET_ZERO: u8 = 0x03;
/// 清除故障闩锁。
pub const CMD_CLEAR_FAULT: u8 = 0x04;
/// 启动编码器校准。
pub const CMD_START_CALI: u8 = 0x05;
/// 切换外环模式。
pub const CMD_SET_MODE: u8 = 0x06;
/// 修改节点号，成功后电机会复位。
pub const CMD_SET_NODE_ID: u8 = 0x07;
/// 保存速度与位置增益到 Flash。
pub const CMD_SAVE_USER_PARAMS: u8 = 0x08;
/// 查询板卡、能力与固件版本。
pub const CMD_GET_DEVICE_INFO: u8 = 0x09;
/// 切换电压 / 电流内环。
pub const CMD_SET_INNER_LOOP: u8 = 0x0A;
/// 查询状态。
pub const CMD_GET_STATUS: u8 = 0x10;
/** @brief 请求一帧运动反馈 0x300，不回 ACK。需要能力 bit4。 */
pub const CMD_GET_FEEDBACK: u8 = 0x11;
/// 写一组增益，只改 RAM。
pub const CMD_SET_GAINS: u8 = 0x20;
/// 读一组增益。
pub const CMD_GET_GAINS: u8 = 0x21;
/// 读电流配置页。参数字节为页号 0 或 1。
pub const CMD_GET_CURRENT_CONFIG: u8 = 0x22;
/// 读实测电流。
pub const CMD_GET_CURRENT_STATE: u8 = 0x23;

/// Motion 命令基址。
pub const CAN_MOTION: u32 = 0x100;
/// Direct Iq 命令基址，D0..1 为有符号小端 mA。
pub const CAN_IQ_CMD: u32 = 0x4C0;
/// 速度命令基址。
pub const CAN_VEL: u32 = 0x140;
/// 增益命令与应答基址。
pub const CAN_GAINS: u32 = 0x180;
/// 位置命令基址。
pub const CAN_POS: u32 = 0x1C0;
/// 管理命令基址。
pub const CAN_MGMT: u32 = 0x200;
/// 命令 ACK 基址。
pub const CAN_ACK: u32 = 0x280;
/// 运动反馈基址。
pub const CAN_FB: u32 = 0x300;
/// 状态应答基址。
pub const CAN_STATUS: u32 = 0x380;
/// 校准上报基址。
pub const CAN_CALI: u32 = 0x3C0;
/// 电流实测应答基址。
pub const CAN_CURRENT: u32 = 0x400;
/// 电流配置应答基址。
pub const CAN_CURRENT_CONFIG: u32 = 0x440;
/** @brief 广播管理帧 ID（节点号 0），只接受 DISABLE。 */
pub const CAN_BROADCAST: u32 = CAN_MGMT;

/// 能力：第二编码器。
pub const CAP_ENCODER2: u8 = 0x01;
/// 能力：电流采样硬件。
pub const CAP_CURRENT: u8 = 0x02;
/// 能力：工厂固件。
pub const CAP_FACTORY: u8 = 0x04;
/// 能力：电流协议（bit3）。未置位时不能把 pu 解释成安培。
pub const CAP_CURRENT_LOOP: u8 = 0x08;
/** @brief 能力 bit4：DISABLE、广播停止、GET_FEEDBACK 与故障主动上报。 */
pub const CAP_CONTROL_EXT: u8 = 0x10;
/**
 * @brief 能力 bit5：状态帧 Byte7 的 bit4 是本固件的多圈位置未建立警告
 *        （原协议该位叫 POSITION_LIMIT_WARNING）。未置位时不把 bit4 当作 ABS。
 */
pub const CAP_POSITION_WARN: u8 = 0x20;
/** @brief 能力 bit6：上电探测到 NTC，状态帧温度字段有效。 */
pub const CAP_TEMP_SENSOR: u8 = 0x40;

/** @brief 警告 bit0：板温达到阈值。与原协议 MOTOR_TEMPERATURE_WARNING 同位。 */
pub const WARN_TEMP: u8 = 0x01;
/**
 * @brief 警告 bit4：双编码器板上电定圈失败，位置只在零点所在的那一圈内有效。
 *        需能力 bit5；原协议该位叫 POSITION_LIMIT_WARNING。
 */
pub const WARN_ABS_UNKNOWN: u8 = 0x10;

/// 直接电流指令的软件上限，A。实际限幅还要和设备 `i_limit` 取较小值。
pub const HOST_CURRENT_CAP_A: f64 = 20.0;

const FAULT_BITS: &[(u8, &str)] = &[
    (0, "OVER_TEMPERATURE"),
    (1, "OVER_CURRENT"),
    (2, "OVER_VOLTAGE"),
    (3, "UNDER_VOLTAGE"),
    (4, "ENCODER_FAULT"),
    (5, "STALL_OVERLOAD"),
    (6, "DRIVER_FAULT"),
    (8, "POSITION_ERROR"),
    (9, "MOTOR_PHASE_FAULT"),
    (10, "CALIBRATION_FAULT"),
    (11, "CURRENT_SAMPLE_FAULT"),
];

/// 一个节点的标准帧 ID。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ids {
    /// Motion `0x100 + id`。
    pub motion: u32,
    /// Direct Iq `0x4C0 + id`。
    pub iq_cmd: u32,
    /// Velocity `0x140 + id`。
    pub vel: u32,
    /// Gains `0x180 + id`。
    pub gains: u32,
    /// Position `0x1C0 + id`。
    pub pos: u32,
    /// Management `0x200 + id`。
    pub mgmt: u32,
    /// ACK `0x280 + id`。
    pub ack: u32,
    /// Feedback `0x300 + id`。
    pub fb: u32,
    /// Status `0x380 + id`。
    pub status: u32,
    /// Calibration `0x3C0 + id`。
    pub cali: u32,
    /// Current state `0x400 + id`。
    pub current: u32,
    /// Current config `0x440 + id`。
    pub current_config: u32,
}

/// 把节点号夹到 1..=63 并生成各帧 ID。
pub fn ids(node_id: u8) -> Ids {
    let nid = node_id.clamp(1, 63) as u32;
    Ids {
        motion: CAN_MOTION + nid,
        iq_cmd: CAN_IQ_CMD + nid,
        vel: CAN_VEL + nid,
        gains: CAN_GAINS + nid,
        pos: CAN_POS + nid,
        mgmt: CAN_MGMT + nid,
        ack: CAN_ACK + nid,
        fb: CAN_FB + nid,
        status: CAN_STATUS + nid,
        cali: CAN_CALI + nid,
        current: CAN_CURRENT + nid,
        current_config: CAN_CURRENT_CONFIG + nid,
    }
}

/// 标准帧属于哪个节点。扩展帧或节点号不在 1..=63 时返回 `None`。
pub fn node_of(arbitration_id: u32) -> Option<u8> {
    if arbitration_id > 0x7FF {
        return None;
    }
    let node = (arbitration_id & 0x3F) as u8;
    if (1..=63).contains(&node) {
        Some(node)
    } else {
        None
    }
}

/// 主机发出的实时控制帧（Motion / Velocity / Position / Direct Iq）。
pub fn is_realtime(arbitration_id: u32) -> bool {
    let base = arbitration_id & !0x3F;
    matches!(base, CAN_MOTION | CAN_VEL | CAN_POS | CAN_IQ_CMD)
}

/// 电机上行帧种类。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RxKind {
    /// `0x180` 增益读回。
    Gains,
    /// `0x280` 命令 ACK。
    Ack,
    /// `0x300` 运动反馈。
    Feedback,
    /// `0x380` 状态。
    Status,
    /// `0x3C0` 校准上报。
    Calibration,
    /// `0x400` 电流实测。
    Current,
    /// `0x440` 电流配置。
    CurrentConfig,
}

/// 由仲裁 ID 判断上行帧种类。
pub fn rx_kind(arbitration_id: u32) -> Option<RxKind> {
    match arbitration_id & !0x3F {
        CAN_GAINS => Some(RxKind::Gains),
        CAN_ACK => Some(RxKind::Ack),
        CAN_FB => Some(RxKind::Feedback),
        CAN_STATUS => Some(RxKind::Status),
        CAN_CALI => Some(RxKind::Calibration),
        CAN_CURRENT => Some(RxKind::Current),
        CAN_CURRENT_CONFIG => Some(RxKind::CurrentConfig),
        _ => None,
    }
}

/// 外环模式名称。未知值返回十六进制。
pub fn mode_name(mode: u8) -> String {
    match mode {
        MODE_MOTION => "MOTION".into(),
        MODE_VELOCITY => "VELOCITY".into(),
        MODE_POSITION => "POSITION".into(),
        other => format!("0x{other:02X}"),
    }
}

/// 设备状态名称。
pub fn state_name(state: u8) -> String {
    match state {
        0x00 => "DISABLED".into(),
        0x01 => "READY".into(),
        0x02 => "RUNNING".into(),
        0x03 => "FAULT".into(),
        0x04 => "CALIBRATING".into(),
        other => format!("0x{other:02X}"),
    }
}

/// ACK 结果名称。
pub fn result_name(result: u8) -> String {
    match result {
        0x00 => "OK".into(),
        0x01 => "INVALID_COMMAND".into(),
        0x02 => "INVALID_STATE".into(),
        0x03 => "PARAMETER_OUT_OF_RANGE".into(),
        0x04 => "FAULT_ACTIVE".into(),
        0x05 => "BUSY".into(),
        0x06 => "INTERNAL_ERROR".into(),
        other => format!("0x{other:02X}"),
    }
}

/// 管理命令名称。
pub fn cmd_name(cmd: u8) -> String {
    match cmd {
        CMD_ENABLE => "ENABLE".into(),
        CMD_DISABLE => "DISABLE".into(),
        CMD_SET_ZERO => "SET_ZERO".into(),
        CMD_CLEAR_FAULT => "CLEAR_FAULT".into(),
        CMD_START_CALI => "START_CALI".into(),
        CMD_SET_MODE => "SET_CONTROL_MODE".into(),
        CMD_SET_NODE_ID => "SET_NODE_ID".into(),
        CMD_SAVE_USER_PARAMS => "SAVE_USER_PARAMS".into(),
        CMD_GET_DEVICE_INFO => "GET_DEVICE_INFO".into(),
        CMD_SET_INNER_LOOP => "SET_INNER_LOOP".into(),
        CMD_GET_STATUS => "GET_STATUS".into(),
        CMD_GET_FEEDBACK => "GET_FEEDBACK".into(),
        CMD_SET_GAINS => "SET_GAINS".into(),
        CMD_GET_GAINS => "GET_GAINS".into(),
        CMD_GET_CURRENT_CONFIG => "GET_CURRENT_CONFIG".into(),
        CMD_GET_CURRENT_STATE => "GET_CURRENT_STATE".into(),
        other => format!("0x{other:02X}"),
    }
}

/// 内环名称。`None` 表示旧固件，不能从状态字节推断。
pub fn inner_loop_name(inner: Option<u8>) -> &'static str {
    match inner {
        Some(INNER_LOOP_VOLTAGE) => "VOLTAGE",
        Some(INNER_LOOP_CURRENT) => "CURRENT",
        Some(_) => "UNKNOWN",
        None => "UNKNOWN",
    }
}

/// 板卡号名称。
pub fn board_name(board_id: u8) -> String {
    match board_id {
        1 => "YH_JM".into(),
        other => format!("board {other}"),
    }
}

/// 故障位名称。无故障返回 `NONE`，有未知位时返回十六进制。
pub fn fault_name(fault: u16) -> String {
    if fault == 0 {
        return "NONE".into();
    }
    let names: Vec<&str> = FAULT_BITS
        .iter()
        .filter(|(bit, _)| (fault >> bit) & 1 == 1)
        .map(|(_, name)| *name)
        .collect();
    if names.is_empty() {
        format!("0x{fault:04X}")
    } else {
        names.join(",")
    }
}

/**
 * @brief 警告位名称。
 *
 * bit0 按原协议解释为温度警告；bit4 在已确认能力 bit5 时解释为多圈位置未建立，
 * 调用方应先按能力屏蔽不该解释的位。无警告返回 `NONE`，其余位以十六进制附在后面。
 */
pub fn warn_name(warn: u8) -> String {
    let warn = warn & 0x1F;
    if warn == 0 {
        return "NONE".into();
    }
    let mut names: Vec<String> = Vec::new();
    if warn & WARN_TEMP != 0 {
        names.push("MOTOR_TEMPERATURE_WARNING".into());
    }
    if warn & WARN_ABS_UNKNOWN != 0 {
        names.push("ABS_POSITION_UNKNOWN".into());
    }
    let other = warn & !(WARN_TEMP | WARN_ABS_UNKNOWN);
    if other != 0 {
        names.push(format!("0x{other:02X}"));
    }
    names.join(",")
}

fn clamp_i16(v: i64) -> i16 {
    v.clamp(i16::MIN as i64, i16::MAX as i64) as i16
}

fn clamp_u16(v: i64) -> u16 {
    v.clamp(0, u16::MAX as i64) as u16
}

fn round_i64(v: f64) -> i64 {
    v.round() as i64
}

/// Motion 命令：位置 int32、速度 int16、输出轴力矩 int16（0.001 Nm）。
pub fn pack_motion(pos: f64, vel: f64, torque_nm: f64) -> [u8; 8] {
    let pos_raw = round_i64(pos / POS_LSB).clamp(i32::MIN as i64, i32::MAX as i64) as i32;
    let vel_raw = clamp_i16(round_i64(vel / VEL_LSB));
    let torque_nm = torque_nm.clamp(-32767.0 * TORQUE_LSB, 32767.0 * TORQUE_LSB);
    let ff_raw = clamp_i16(round_i64(torque_nm / TORQUE_LSB));
    let mut data = [0u8; 8];
    data[0..4].copy_from_slice(&pos_raw.to_le_bytes());
    data[4..6].copy_from_slice(&vel_raw.to_le_bytes());
    data[6..8].copy_from_slice(&ff_raw.to_le_bytes());
    data
}

/// 速度模式命令。后 4 字节为 0。
pub fn pack_velocity(vel: f64) -> [u8; 8] {
    let vel_raw = round_i64(vel / VEL_LSB).clamp(i32::MIN as i64, i32::MAX as i64) as i32;
    let mut data = [0u8; 8];
    data[0..4].copy_from_slice(&vel_raw.to_le_bytes());
    data
}

/// 位置模式命令：目标位置与最大速度。
pub fn pack_position(pos: f64, max_vel: f64) -> [u8; 8] {
    let pos_raw = round_i64(pos / POS_LSB).clamp(i32::MIN as i64, i32::MAX as i64) as i32;
    let max_raw = round_i64(max_vel.abs() / VEL_LSB).clamp(0, u32::MAX as i64) as u32;
    let mut data = [0u8; 8];
    data[0..4].copy_from_slice(&pos_raw.to_le_bytes());
    data[4..8].copy_from_slice(&max_raw.to_le_bytes());
    data
}

/// 增益帧。Motion 强制 Ki=0，速度模式强制 Kd=0。
pub fn pack_gains(mode: u8, seq: u8, kp: f64, ki: f64, kd: f64) -> [u8; 8] {
    let (kp_raw, ki_raw, kd_raw) = match mode {
        MODE_MOTION => (
            clamp_u16(round_i64(kp / KP_LSB_MOTION)),
            0,
            clamp_u16(round_i64(kd / KD_LSB_MOTION)),
        ),
        MODE_VELOCITY => (
            clamp_u16(round_i64(kp / KP_LSB_VEL)),
            clamp_u16(round_i64(ki / KI_LSB_VEL)),
            0,
        ),
        _ => (
            clamp_u16(round_i64(kp / KP_LSB_POS)),
            clamp_u16(round_i64(ki / KI_LSB_POS)),
            clamp_u16(round_i64(kd / KD_LSB_POS)),
        ),
    };
    let mut data = [0u8; 8];
    data[0] = mode;
    data[1] = seq;
    data[2..4].copy_from_slice(&kp_raw.to_le_bytes());
    data[4..6].copy_from_slice(&ki_raw.to_le_bytes());
    data[6..8].copy_from_slice(&kd_raw.to_le_bytes());
    data
}

/// 解出的增益。
#[derive(Clone, Debug, PartialEq)]
pub struct Gains {
    /// 模式字节。
    pub mode: u8,
    /// 模式名称。
    pub mode_name: String,
    /// 序号。
    pub seq: u8,
    /// Kp，单位随模式变化。
    pub kp: f64,
    /// Ki。Motion 恒为 0。
    pub ki: f64,
    /// Kd。速度模式恒为 0。
    pub kd: f64,
}

/// 解析 8 字节增益帧。
pub fn unpack_gains(data: &[u8; 8]) -> Gains {
    let mode = data[0];
    let seq = data[1];
    let kp_raw = u16::from_le_bytes([data[2], data[3]]);
    let ki_raw = u16::from_le_bytes([data[4], data[5]]);
    let kd_raw = u16::from_le_bytes([data[6], data[7]]);
    let (kp, ki, kd) = match mode {
        MODE_MOTION => (
            kp_raw as f64 * KP_LSB_MOTION,
            0.0,
            kd_raw as f64 * KD_LSB_MOTION,
        ),
        MODE_VELOCITY => (
            kp_raw as f64 * KP_LSB_VEL,
            ki_raw as f64 * KI_LSB_VEL,
            0.0,
        ),
        _ => (
            kp_raw as f64 * KP_LSB_POS,
            ki_raw as f64 * KI_LSB_POS,
            kd_raw as f64 * KD_LSB_POS,
        ),
    };
    Gains {
        mode,
        mode_name: mode_name(mode),
        seq,
        kp,
        ki,
        kd,
    }
}

/// 管理帧。`arg` 写入 Byte2，其余为 0。
pub fn pack_mgmt(cmd: u8, seq: u8, arg: u8) -> [u8; 8] {
    let mut data = [0u8; 8];
    data[0] = cmd;
    data[1] = seq;
    data[2] = arg;
    data
}

/// 运动反馈。`torque` 是运控指令换算的输出轴力矩，不是实测转矩。
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Feedback {
    /// 多圈位置，rad。
    pub pos: f64,
    /// 速度，rad/s。
    pub vel: f64,
    /// 输出轴指令力矩，Nm。
    pub torque: f64,
}

/// 解析运动反馈。
pub fn unpack_feedback(data: &[u8; 8]) -> Feedback {
    let pos_raw = i32::from_le_bytes([data[0], data[1], data[2], data[3]]);
    let vel_raw = i16::from_le_bytes([data[4], data[5]]);
    let torque_raw = i16::from_le_bytes([data[6], data[7]]);
    Feedback {
        pos: pos_raw as f64 * POS_LSB,
        vel: vel_raw as f64 * VEL_LSB,
        torque: torque_raw as f64 * TORQUE_LSB,
    }
}

/// 命令 ACK。
#[derive(Clone, Debug, PartialEq)]
pub struct Ack {
    /// 被应答的命令。
    pub cmd: u8,
    /// 命令名称。
    pub cmd_name: String,
    /// 序号。
    pub seq: u8,
    /// 结果码，0 为成功。
    pub result: u8,
    /// 结果名称。
    pub result_name: String,
    /// 设备状态。
    pub state: u8,
    /// 状态名称。
    pub state_name: String,
    /// 故障位。
    pub fault: u16,
    /// 故障名称。
    pub fault_name: String,
    /// `SET_NODE_ID` 成功时的新节点号。
    pub new_id: u8,
}

/// 解析 ACK。
pub fn unpack_ack(data: &[u8; 8]) -> Ack {
    let cmd = data[0];
    let result = data[2];
    let state = data[3];
    let fault = u16::from_le_bytes([data[4], data[5]]);
    Ack {
        cmd,
        cmd_name: cmd_name(cmd),
        seq: data[1],
        result,
        result_name: result_name(result),
        state,
        state_name: state_name(state),
        fault,
        fault_name: fault_name(fault),
        new_id: data[6],
    }
}

/// `GET_DEVICE_INFO` 成功应答。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeviceInfo {
    /// 序号。
    pub seq: u8,
    /// 结果码。
    pub result: u8,
    /// 板卡号。
    pub board_id: u8,
    /// 板卡名称。
    pub board_name: String,
    /// 能力字节。
    pub caps: u8,
    /// 第二编码器。
    pub encoder2: bool,
    /// 电流采样硬件。
    pub current: bool,
    /// 工厂固件。
    pub factory: bool,
    /// 电流协议 bit3。
    pub current_loop: bool,
    /** @brief 运控扩展 bit4。 */
    pub control_ext: bool,
    /** @brief 位置警告 bit5：Byte7 bit4 按 ABS_POSITION_UNKNOWN 解释。 */
    pub position_warn: bool,
    /** @brief 温度传感器 bit6：状态帧温度字段有效。 */
    pub temp_sensor: bool,
    /// 固件主版本。
    pub fw_major: u8,
    /// 固件次版本。
    pub fw_minor: u8,
}

/// 解析设备信息。调用方应先确认这是成功的 `GET_DEVICE_INFO` ACK。
pub fn unpack_device_info(data: &[u8; 8]) -> DeviceInfo {
    let board_id = data[4];
    let caps = data[5];
    DeviceInfo {
        seq: data[1],
        result: data[2],
        board_id,
        board_name: board_name(board_id),
        caps,
        encoder2: caps & CAP_ENCODER2 != 0,
        current: caps & CAP_CURRENT != 0,
        factory: caps & CAP_FACTORY != 0,
        current_loop: caps & CAP_CURRENT_LOOP != 0,
        control_ext: caps & CAP_CONTROL_EXT != 0,
        position_warn: caps & CAP_POSITION_WARN != 0,
        temp_sensor: caps & CAP_TEMP_SENSOR != 0,
        fw_major: data[6],
        fw_minor: data[7],
    }
}

/// 状态应答。`inner_loop` 仅在已确认电流协议时有效。
#[derive(Clone, Debug, PartialEq)]
pub struct Status {
    /// 状态字节。
    pub state: u8,
    /// 状态名称。
    pub state_name: String,
    /// 故障位。
    pub fault: u16,
    /// 故障名称。
    pub fault_name: String,
    /// 母线电压，V。
    pub vbus: f64,
    /// 温度，°C。无 NTC 或能力 bit6 未置位时为 0。
    pub temp: f64,
    /// 外环模式。
    pub mode: u8,
    /// 模式名称。
    pub mode_name: String,
    /// 内环。旧固件为 `None`。
    pub inner_loop: Option<u8>,
    /// 内环名称。
    pub inner_loop_name: String,
    /// 警告位，Byte7 低 5 位。
    pub warn: u8,
}

/// 解析状态。`supports_current == false` 时不解释 Byte7 bit7。
pub fn unpack_status(data: &[u8; 8], supports_current: bool) -> Status {
    let state = data[0];
    let fault = u16::from_le_bytes([data[1], data[2]]);
    let vbus = u16::from_le_bytes([data[3], data[4]]) as f64 * VBUS_LSB;
    let temp = i16::from_le_bytes([data[5], data[6]]) as f64 * TEMP_LSB;
    let byte7 = data[7];
    let mode = (byte7 >> 5) & 0x03;
    let inner = if supports_current {
        Some((byte7 >> 7) & 1)
    } else {
        None
    };
    Status {
        state,
        state_name: state_name(state),
        fault,
        fault_name: fault_name(fault),
        vbus,
        temp,
        mode,
        mode_name: mode_name(mode),
        inner_loop: inner,
        inner_loop_name: inner_loop_name(inner).into(),
        warn: byte7 & 0x1F,
    }
}

/// 一页电流配置。页 0 是限幅与带宽，页 1 是 PI，页 2 是转矩常数和减速比。
#[derive(Clone, Debug, PartialEq)]
pub struct CurrentConfigPage {
    /// 页号。
    pub page: u8,
    /// 序号。
    pub seq: u8,
    /// 电流限幅，A。仅页 0。
    pub i_limit: Option<f64>,
    /// 标幺限幅，pu。仅页 0。
    pub v_limit: Option<f64>,
    /// 带宽，Hz。仅页 0。
    pub bandwidth_hz: Option<u16>,
    /// Kp，V/A。仅页 1。
    pub kp: Option<f64>,
    /// Ki，V/(A·s)，原始整数。仅页 1。
    pub ki: Option<f64>,
    /// 电流环频率，Hz。仅页 1。
    pub loop_hz: Option<u16>,
    /// 转矩常数，Nm/A。仅页 2。
    pub kt: Option<f64>,
    /// 减速比。仅页 2。
    pub gear: Option<f64>,
    /// 页 2 D6 bit0，支持独立的 Iq 指令。
    pub direct_iq: Option<bool>,
}

/// 解析电流配置页。未知页返回错误。
pub fn unpack_current_config(data: &[u8; 8]) -> Result<CurrentConfigPage, String> {
    let page = data[0];
    let seq = data[1];
    let a = u16::from_le_bytes([data[2], data[3]]);
    let b = u16::from_le_bytes([data[4], data[5]]);
    let c = u16::from_le_bytes([data[6], data[7]]);
    match page {
        0 => Ok(CurrentConfigPage {
            page,
            seq,
            i_limit: Some(a as f64 * 0.001),
            v_limit: Some(b as f64 * 0.0001),
            bandwidth_hz: Some(c),
            kp: None,
            ki: None,
            loop_hz: None,
            kt: None,
            gear: None,
            direct_iq: None,
        }),
        1 => Ok(CurrentConfigPage {
            page,
            seq,
            i_limit: None,
            v_limit: None,
            bandwidth_hz: None,
            kp: Some(a as f64 * 0.001),
            ki: Some(b as f64),
            loop_hz: Some(c),
            kt: None,
            gear: None,
            direct_iq: None,
        }),
        2 => Ok(CurrentConfigPage {
            page,
            seq,
            i_limit: None,
            v_limit: None,
            bandwidth_hz: None,
            kp: None,
            ki: None,
            loop_hz: None,
            kt: Some(a as f64 * 0.001),
            gear: Some(b as f64 * 0.01),
            direct_iq: Some(c & 1 != 0),
        }),
        other => Err(format!("Unknown current configuration page {other}")),
    }
}

/// 电流实测与指令。
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CurrentState {
    /// 实测 Id，A。
    pub id: f64,
    /// 实测 Iq，A。
    pub iq: f64,
    /// 指令 Iq，A。有符号，以本帧为准。
    pub iq_ref: f64,
    /// PI 输出 Vq，pu。
    pub vq: f64,
}

/// 解析电流实测帧。
pub fn unpack_current_state(data: &[u8; 8]) -> CurrentState {
    let id_raw = i16::from_le_bytes([data[0], data[1]]);
    let iq_raw = i16::from_le_bytes([data[2], data[3]]);
    let ref_raw = i16::from_le_bytes([data[4], data[5]]);
    let vq_raw = i16::from_le_bytes([data[6], data[7]]);
    CurrentState {
        id: id_raw as f64 * 0.001,
        iq: iq_raw as f64 * 0.001,
        iq_ref: ref_raw as f64 * 0.001,
        vq: vq_raw as f64 * 0.0001,
    }
}

/// 已收齐的电流配置，供换算与界面使用。
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CurrentConfig {
    /// 电流限幅，A。
    pub i_limit: Option<f64>,
    /// 标幺限幅，pu。
    pub v_limit: Option<f64>,
    /// 带宽，Hz。
    pub bandwidth_hz: Option<u16>,
    /// Kp，V/A。
    pub kp: Option<f64>,
    /// Ki，V/(A·s)。
    pub ki: Option<f64>,
    /// 环频率，Hz。
    pub loop_hz: Option<u16>,
    /// 转矩常数，Nm/A。
    pub kt: Option<f64>,
    /// 减速比。
    pub gear: Option<f64>,
    /// 页 2 D6 bit0，支持独立的 Iq 指令。
    pub direct_iq: Option<bool>,
}

impl CurrentConfig {
    /// 合并一页配置。
    pub fn apply_page(&mut self, page: &CurrentConfigPage) {
        if let Some(v) = page.i_limit {
            self.i_limit = Some(v);
        }
        if let Some(v) = page.v_limit {
            self.v_limit = Some(v);
        }
        if let Some(v) = page.bandwidth_hz {
            self.bandwidth_hz = Some(v);
        }
        if let Some(v) = page.kp {
            self.kp = Some(v);
        }
        if let Some(v) = page.ki {
            self.ki = Some(v);
        }
        if let Some(v) = page.loop_hz {
            self.loop_hz = Some(v);
        }
        if let Some(v) = page.kt {
            self.kt = Some(v);
        }
        if page.direct_iq.is_some() {
            self.direct_iq = page.direct_iq;
        }
        if let Some(v) = page.gear {
            self.gear = Some(v);
        }
    }

    /// 两页是否都到了，可以计算 A/pu。
    pub fn has_scale(&self) -> bool {
        self.i_limit.is_some() && self.v_limit.is_some()
    }
}

/// 该设备的 A/pu。只在配置页 0 有效之后可用。
pub fn current_scale(config: Option<&CurrentConfig>) -> Result<f64, String> {
    let config = config.ok_or("尚未读取设备电流配置")?;
    let limit = config.i_limit.unwrap_or(0.0);
    let voltage_limit = config.v_limit.unwrap_or(0.0);
    if !(limit.is_finite() && voltage_limit.is_finite() && limit > 0.0 && voltage_limit > 0.0 && voltage_limit <= 1.0)
    {
        return Err("设备返回的电流配置无效".into());
    }
    Ok(limit / voltage_limit)
}

/// 编码直接 Iq，零电流也使用本帧。只有固件明确支持时才能发送。
pub fn pack_direct_iq(amps: f64, config: Option<&CurrentConfig>) -> Result<[u8; 8], String> {
    if config.and_then(|cfg| cfg.direct_iq) != Some(true) {
        return Err("设备未声明独立 Iq 指令能力，请读取配置或升级固件".into());
    }
    validate_current_amps(amps, config)?;
    let mut data = [0; 8];
    data[..2].copy_from_slice(&((amps * 1000.0).round() as i16).to_le_bytes());
    Ok(data)
}

/// 根据电机参数估算等效输出轴力矩。发送直接 Iq 使用 `pack_direct_iq`。电流限幅与 `current_command_to_pu` 相同，再乘转矩常数和减速比。
pub fn current_command_to_nm(amperes: f64, config: Option<&CurrentConfig>) -> Result<f64, String> {
    let _pu = current_command_to_pu(amperes, config)?;
    let config = config.ok_or("尚未读取设备电流配置")?;
    let kt = config.kt.unwrap_or(0.0);
    let gear = config.gear.unwrap_or(0.0);
    if !(kt.is_finite() && gear.is_finite() && kt > 0.0 && gear > 0.0) {
        return Err("设备返回的转矩常数或减速比无效".into());
    }
    Ok(amperes * kt * gear)
}

/// 把安培指令换成 Motion 前馈 pu。允许范围是设备限幅与 `HOST_CURRENT_CAP_A` 的较小值。
pub fn current_command_to_pu(amperes: f64, config: Option<&CurrentConfig>) -> Result<f64, String> {
    let scale = current_scale(config)?;
    validate_current_amps(amperes, config)?;
    Ok(amperes / scale)
}

fn validate_current_amps(amperes: f64, config: Option<&CurrentConfig>) -> Result<(), String> {
    let rated = config.and_then(|c| c.i_limit).ok_or("尚未读取设备电流上限")?;
    if !rated.is_finite() || rated <= 0.0 {
        return Err("设备返回的电流上限无效".into());
    }
    let limit = rated.min(HOST_CURRENT_CAP_A);
    if !amperes.is_finite() || amperes.abs() > limit {
        return Err(format!("电流指令须在 ±{limit} A 内"));
    }
    Ok(())
}

/// 校准上报。
#[derive(Clone, Debug, PartialEq)]
pub struct Calibration {
    /// 序号。上电自动校准时可能为 0。
    pub seq: u8,
    /// 状态字节。
    pub state: u8,
    /// 状态名称。
    pub state_name: String,
    /// 进度，百分比。
    pub progress: u8,
    /// 阶段字节。
    pub stage: u8,
    /// 阶段名称。
    pub stage_name: String,
    /// 错误码。
    pub error: u16,
    /// 错误名称。
    pub error_name: String,
}

/// 解析校准上报。
pub fn unpack_cali(data: &[u8; 8]) -> Calibration {
    let state = data[1];
    let stage = data[3];
    let error = u16::from_le_bytes([data[4], data[5]]);
    Calibration {
        seq: data[0],
        state,
        state_name: match state {
            0x00 => "IDLE".into(),
            0x01 => "RUNNING".into(),
            0x02 => "SUCCESS".into(),
            0x03 => "FAILED".into(),
            other => format!("0x{other:02X}"),
        },
        progress: data[2],
        stage,
        stage_name: match stage {
            0x00 => "IDLE".into(),
            0x01 => "PRE_CHECK".into(),
            0x02 => "MOTOR_ALIGNMENT".into(),
            0x03 => "ENCODER_SAMPLING".into(),
            0x04 => "PARAMETER_CALCULATION".into(),
            0x05 => "PARAMETER_SAVE".into(),
            0x06 => "COMPLETE".into(),
            other => format!("0x{other:02X}"),
        },
        error,
        error_name: match error {
            0x0000 => "NO_ERROR".into(),
            0x0001 => "INVALID_STATE".into(),
            0x0002 => "ENCODER_SIGNAL_ERROR".into(),
            0x0003 => "CALIBRATION_TIMEOUT".into(),
            0x0004 => "CALIBRATION_DATA_INVALID".into(),
            0x0005 => "PARAMETER_SAVE_FAILED".into(),
            0x0006 => "MOTOR_CONTROL_ERROR".into(),
            0x00FF => "INTERNAL_ERROR".into(),
            other => format!("0x{other:04X}"),
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pack_config(page: u8, seq: u8, a: u16, b: u16, c: u16) -> [u8; 8] {
        let mut data = [0u8; 8];
        data[0] = page;
        data[1] = seq;
        data[2..4].copy_from_slice(&a.to_le_bytes());
        data[4..6].copy_from_slice(&b.to_le_bytes());
        data[6..8].copy_from_slice(&c.to_le_bytes());
        data
    }

    #[test]
    fn config_pages_and_nondefault_scale() {
        let mut config = CurrentConfig::default();
        config.apply_page(&unpack_current_config(&pack_config(0, 17, 2000, 5000, 500)).unwrap());
        config.apply_page(&unpack_current_config(&pack_config(1, 18, 7477, 16211, 20000)).unwrap());
        assert_eq!(config.i_limit, Some(2.0));
        assert_eq!(config.kp, Some(7.477));
        assert_eq!(config.ki, Some(16211.0));
        assert_eq!(current_scale(Some(&config)).unwrap(), 4.0);
        assert_eq!(current_command_to_pu(1.0, Some(&config)).unwrap(), 0.25);
        config.kt = Some(0.2);
        config.gear = Some(1.0);
        let data = pack_motion(0.0, 0.0, current_command_to_nm(-1.0, Some(&config)).unwrap());
        let ff = i16::from_le_bytes([data[6], data[7]]) as f64 * TORQUE_LSB;
        assert!((ff + 0.2).abs() < TORQUE_LSB);
    }

    #[test]
    fn direct_iq_uses_signed_milliamps_and_advertised_capability() {
        let mut config = CurrentConfig::default();
        config.apply_page(&unpack_current_config(&pack_config(0, 1, 2000, 4500, 500)).unwrap());
        assert!(pack_direct_iq(0.0, Some(&config)).is_err());
        for flags in [0, 2, 1] {
            config.apply_page(&unpack_current_config(&pack_config(2, 2, 0, 0, flags)).unwrap());
            assert_eq!(pack_direct_iq(0.0, Some(&config)).is_ok(), flags == 1);
        }
        assert_eq!(pack_direct_iq(-0.125, Some(&config)).unwrap(), [0x83, 0xff, 0, 0, 0, 0, 0, 0]);
        assert_eq!(pack_direct_iq(0.0, Some(&config)).unwrap(), [0; 8]);
        assert_eq!(&pack_direct_iq(2.0, Some(&config)).unwrap()[..2], &2000i16.to_le_bytes());
        for amps in [2.001, -2.001, f64::NAN, f64::INFINITY] {
            assert!(pack_direct_iq(amps, Some(&config)).is_err());
        }
        assert_eq!(ids(63).iq_cmd, 0x4ff);
        assert!(is_realtime(ids(1).iq_cmd));
    }

    #[test]
    fn unknown_invalid_config_and_limits() {
        for config in [None, Some(CurrentConfig::default())] {
            assert!(current_command_to_pu(1.0, config.as_ref()).is_err());
        }
        let mut bad = CurrentConfig {
            i_limit: Some(5.0),
            v_limit: Some(0.0),
            ..CurrentConfig::default()
        };
        assert!(current_command_to_pu(1.0, Some(&bad)).is_err());
        bad.i_limit = Some(f64::NAN);
        bad.v_limit = Some(0.5);
        assert!(current_command_to_pu(1.0, Some(&bad)).is_err());

        for (amps, limit) in [(2.001, 2.0), (10.001, 10.0), (f64::NAN, 5.0), (f64::INFINITY, 5.0)] {
            let config = CurrentConfig {
                i_limit: Some(limit),
                v_limit: Some(0.5),
                ..CurrentConfig::default()
            };
            assert!(current_command_to_pu(amps, Some(&config)).is_err());
        }
        let five = CurrentConfig {
            i_limit: Some(5.0),
            v_limit: Some(0.5),
            ..CurrentConfig::default()
        };
        assert_eq!(current_command_to_pu(5.0, Some(&five)).unwrap(), 0.5);
        let ten = CurrentConfig {
            i_limit: Some(10.0),
            v_limit: Some(0.5),
            ..CurrentConfig::default()
        };
        assert_eq!(current_command_to_pu(10.0, Some(&ten)).unwrap(), 0.5);
    }

    #[test]
    fn signed_current_telemetry() {
        let mut data = [0u8; 8];
        data[0..2].copy_from_slice(&(-1234i16).to_le_bytes());
        data[2..4].copy_from_slice(&2345i16.to_le_bytes());
        data[4..6].copy_from_slice(&(-5000i16).to_le_bytes());
        data[6..8].copy_from_slice(&(-1234i16).to_le_bytes());
        let result = unpack_current_state(&data);
        assert_eq!(result.id, -1.234);
        assert_eq!(result.iq, 2.345);
        assert_eq!(result.iq_ref, -5.0);
        assert!((result.vq - -0.1234).abs() < 1e-12);
    }

    #[test]
    fn legacy_status_never_infers_inner_loop() {
        for mode in 0..3u8 {
            for inner in 0..2u8 {
                let mut frame = [0u8; 8];
                frame[0] = 2;
                frame[3..5].copy_from_slice(&2400u16.to_le_bytes());
                frame[7] = mode * 32 + inner * 128;
                let legacy = unpack_status(&frame, false);
                let modern = unpack_status(&frame, true);
                assert!(legacy.inner_loop.is_none());
                assert_eq!(legacy.mode, mode);
                assert_eq!(legacy.warn, 0);
                assert_eq!(modern.inner_loop, Some(inner));
            }
        }
    }

    #[test]
    fn capability_and_fault_names() {
        assert!(!unpack_device_info(&[9, 1, 0, 2, 1, 3, 1, 1]).current_loop);
        assert!(unpack_device_info(&[9, 1, 0, 2, 1, 11, 1, 2]).current_loop);
        assert!(fault_name(1 << 11).contains("CURRENT_SAMPLE_FAULT"));
        assert!(fault_name(2).contains("OVER_CURRENT"));
        assert!(!unpack_device_info(&[9, 1, 0, 2, 1, 0x1B, 1, 3]).position_warn);
        assert!(unpack_device_info(&[9, 1, 0, 2, 1, 0x3B, 1, 4]).position_warn);
        assert!(!unpack_device_info(&[9, 1, 0, 2, 1, 0x3B, 1, 4]).temp_sensor);
        assert!(unpack_device_info(&[9, 1, 0, 2, 1, 0x7B, 1, 4]).temp_sensor);
    }

    #[test]
    fn warning_bits() {
        let mut frame = [0u8; 8];
        frame[0] = 2;
        frame[7] = 0x80 | 0x20 | WARN_TEMP | WARN_ABS_UNKNOWN;
        let status = unpack_status(&frame, true);
        assert_eq!(status.warn, WARN_TEMP | WARN_ABS_UNKNOWN);
        assert_eq!(status.mode, 1);
        assert_eq!(status.inner_loop, Some(1));
        assert_eq!(warn_name(0), "NONE");
        assert_eq!(warn_name(WARN_TEMP), "MOTOR_TEMPERATURE_WARNING");
        assert_eq!(warn_name(WARN_ABS_UNKNOWN), "ABS_POSITION_UNKNOWN");
        assert_eq!(
            warn_name(status.warn),
            "MOTOR_TEMPERATURE_WARNING,ABS_POSITION_UNKNOWN"
        );
        assert_eq!(warn_name(0x12), "ABS_POSITION_UNKNOWN,0x02");
    }

    #[test]
    fn node_id_mask_roundtrip() {
        for node in 1..=63u8 {
            let ids = ids(node);
            for id in [ids.motion, ids.vel, ids.gains, ids.pos, ids.mgmt, ids.ack, ids.fb, ids.status, ids.cali, ids.current, ids.current_config] {
                assert_eq!(node_of(id), Some(node));
            }
            assert!(is_realtime(ids.motion) && is_realtime(ids.vel) && is_realtime(ids.pos));
            assert!(!is_realtime(ids.mgmt));
        }
    }
}
