//! 一条总线上的多节点会话。
//!
//! 每个节点单独保存能力、内环、电流配置和循环发送。实时帧只保留最新目标，不排队补发。
//! 直接电流停止或断开时同步发 0 A；普通运控停止只停发。

use std::collections::{BTreeMap, HashMap, VecDeque};
use std::time::Duration;

use yfoc_can::{CanBus, Frame};
use yfoc_protocol::{
    pack_direct_iq, current_scale, fault_name, ids, is_realtime, mode_name, node_of,
    pack_gains, pack_mgmt, pack_motion, pack_position, pack_velocity, result_name, rx_kind,
    state_name, unpack_ack, unpack_cali, unpack_current_config, unpack_current_state,
    unpack_device_info, unpack_feedback, unpack_gains, unpack_status, warn_name, CurrentConfig,
    CurrentState, Gains, RxKind, CAN_BROADCAST, CMD_DISABLE, CMD_GET_CURRENT_CONFIG,
    CMD_GET_CURRENT_STATE, CMD_GET_DEVICE_INFO, CMD_GET_FEEDBACK, CMD_GET_GAINS, CMD_GET_STATUS,
    CMD_SAVE_USER_PARAMS, CMD_SET_INNER_LOOP, CMD_SET_MODE, CMD_SET_NODE_ID,
    CMD_START_CALI, HOST_CURRENT_CAP_A, INNER_LOOP_CURRENT, MODE_MOTION, MODE_POSITION,
    MODE_VELOCITY, WARN_ABS_UNKNOWN, WARN_TEMP,
};

use crate::scope::{Channel, PlotSeries, Scope, ScopeView};

pub const HOST_TITLE: &str = concat!("YHorizon-JM Motor Host ", env!("CARGO_PKG_VERSION"));
const PACE_US: u64 = 2_000;
const SETUP_TIMEOUT_US: u64 = 1_000_000;
const CONFIG_TIMEOUT_US: u64 = 1_000_000;
const SCAN_GRACE_US: u64 = 300_000;
const CONFIRM_DELAY_US: u64 = 500_000;
const STATUS_PERIOD_US: u64 = 1_000_000;
/** @brief 默认状态轮询频率，Hz。 */
pub const DEFAULT_STATUS_HZ: f64 = 2.0;
/** @brief 默认空闲反馈轮询频率，Hz。只对支持 GET_FEEDBACK 且未在循环的节点生效。 */
pub const DEFAULT_FEEDBACK_HZ: f64 = 10.0;
/** @brief 超过这个时间没有任何上行帧，界面显示为失联。 */
const STALE_US: u64 = 3_000_000;
/// 1 Mbps 经典 CAN 大约能稳定跑的帧率。超过 70% 只警告，不丢指令。
const BUS_CAPACITY_HZ: f64 = 7_000.0;

/// 界面或 CLI 发给会话的动作。循环目标在点击时冻结。
#[derive(Clone, Debug)]
pub enum Command {
    /// 选中节点，不改变别的节点缓存。
    Select(u8),
    /// 查询一台设备的能力和状态。
    Query(u8),
    /// 错开查询 1..=63。
    Scan,
    /**
     * @brief 给选中节点下发目标。外环模式不符时先自动 SET_CONTROL_MODE 并等 ACK。
     *
     * 同类循环正在运行时只改写目标，不重启循环。
     */
    Drive { target: Target, cyclic: bool },
    /** @brief 对指定节点执行命令，不改变当前选中。 */
    On(u8, Box<Command>),
    /** @brief 遥测轮询频率，0 表示关闭。 */
    Telemetry { status_hz: f64, current_hz: f64, feedback_hz: f64 },
    /// 停止选中节点。直接电流会补 0 A。
    Stop,
    /// 停止全部循环。只有直接电流的节点补 0 A。
    StopAll,
    /// 写 RAM 增益。
    Gains { mode: u8, kp: f64, ki: f64, kd: f64 },
    /// 读 RAM 增益。
    GetGains { mode: u8 },
    /// 保存速度与位置增益。
    SaveParams,
    /// 不带参数的管理命令。
    Mgmt { cmd: u8 },
    /// 切换外环。先停本节点循环并清实时队列。
    SetMode { mode: u8 },
    /// 改节点号。
    SetNodeId { new_id: u8 },
    /// 切换内环。先停循环，成功后重新读配置。
    InnerLoop { mode: u8 },
    /** @brief 改写选中节点正在循环的目标与频率；类型不符或未在循环时忽略。 */
    Retarget(Target),
    /// 重新读电流配置。
    RefreshCurrent,
    /// 示波开关。
    ScopePause(bool),
    /// 示波跟随。
    ScopeFollow(bool),
    /// 清空曲线。
    ScopeClear,
    /// 时间轴或帧轴。
    ScopeTimeAxis(bool),
    /// 时间窗。
    ScopeWindowSecs(f64),
    /// 帧窗。
    ScopeWindowFrames(f64),
    /// 归一化。
    ScopeNormalize(bool),
    /// 自动纵轴。
    ScopeAutoY(bool),
    /// 通道开关。
    ScopeChannel { index: usize, on: bool },
    /// 设备曲线开关。
    ScopeDevice { id: u8, on: bool },
    /// 绘图宽度。
    ScopeWidth(f32),
    /// 用户当前看到的横轴。
    ScopeManualX { min: f64, max: f64 },
}

impl Command {
    /** @brief 只改示波视图、不碰总线的命令，未连接时也可以执行。 */
    pub fn is_view(&self) -> bool {
        matches!(
            self,
            Command::ScopePause(_)
                | Command::ScopeFollow(_)
                | Command::ScopeClear
                | Command::ScopeTimeAxis(_)
                | Command::ScopeWindowSecs(_)
                | Command::ScopeWindowFrames(_)
                | Command::ScopeNormalize(_)
                | Command::ScopeAutoY(_)
                | Command::ScopeChannel { .. }
                | Command::ScopeDevice { .. }
                | Command::ScopeWidth(_)
                | Command::ScopeManualX { .. }
        )
    }
}

/** @brief 循环发送的帧类型。 */
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CyclicKind {
    /** @brief Motion 帧。 */
    Motion,
    /** @brief Velocity 帧。 */
    Velocity,
    /** @brief Position 帧。 */
    Position,
    /** @brief 直接电流，底层也是 Motion 帧。 */
    Current,
}

/** @brief 循环中途改写的新目标。 */
#[derive(Clone, Copy, Debug)]
pub enum Target {
    /** @brief Motion 目标。 */
    Motion { pos: f64, vel: f64, ff: f64, rate: f64 },
    /** @brief 速度目标。 */
    Velocity { vel: f64, rate: f64 },
    /** @brief 位置目标。 */
    Position { pos: f64, max_vel: f64, rate: f64 },
    /** @brief 电流目标，A。 */
    Current { amps: f64, rate: f64 },
}

impl Target {
    /** @brief 对应的循环类型。 */
    pub fn kind(&self) -> CyclicKind {
        match self {
            Target::Motion { .. } => CyclicKind::Motion,
            Target::Velocity { .. } => CyclicKind::Velocity,
            Target::Position { .. } => CyclicKind::Position,
            Target::Current { .. } => CyclicKind::Current,
        }
    }

    /** @brief 需要的外环模式。直接电流走 MOTION。 */
    pub fn mode(&self) -> u8 {
        match self {
            Target::Velocity { .. } => MODE_VELOCITY,
            Target::Position { .. } => MODE_POSITION,
            Target::Motion { .. } | Target::Current { .. } => MODE_MOTION,
        }
    }

    /** @brief 循环频率。 */
    pub fn rate(&self) -> f64 {
        match *self {
            Target::Motion { rate, .. } | Target::Velocity { rate, .. } | Target::Position { rate, .. } | Target::Current { rate, .. } => rate,
        }
    }

    /** @brief 非电流目标的帧 ID 与数据；电流需要设备配置换算，返回 `None`。 */
    fn frame(&self, id: u8) -> Option<(u32, [u8; 8], String)> {
        match *self {
            Target::Motion { pos, vel, ff, .. } => Some((ids(id).motion, pack_motion(pos, vel, ff), format!("Motion pos={pos} vel={vel} ff={ff}"))),
            Target::Velocity { vel, .. } => Some((ids(id).vel, pack_velocity(vel), format!("Velocity vel={vel}"))),
            Target::Position { pos, max_vel, .. } => {
                Some((ids(id).pos, pack_position(pos, max_vel), format!("Position pos={pos} max_vel={max_vel}")))
            }
            Target::Current { .. } => None,
        }
    }
}

#[derive(Clone, Debug)]
struct Cyclic {
    kind: CyclicKind,
    period_us: u64,
    next_us: u64,
    can_id: u32,
    payload: [u8; 8],
    hz: f64,
}

struct SetupStep {
    cmd: u8,
    frame: Frame,
}

enum SetupDone {
    None,
    Current { amps: f64, data: [u8; 8], cyclic: bool, rate: f64 },
    Drive { target: Target, cyclic: bool },
    Inner,
}

struct SetupWait {
    cmd: u8,
    seq: u8,
    deadline_us: u64,
}

struct Device {
    online: bool,
    board_name: String,
    fw: String,
    supports_current: bool,
    control_ext: bool,
    supports_commission: bool,
    position_warn: bool,
    temp_sensor: bool,
    /** @brief 板温，°C。没探测到 NTC 时为空。 */
    temp: Option<f64>,
    /** @brief 尚未被下一帧确认的母线电压。单次串通道的读数先放这里。 */
    vbus_hold: Option<f64>,
    /** @brief 尚未被下一帧确认的温度。 */
    temp_hold: Option<f64>,
    inner: Option<u8>,
    inner_text: String,
    caps_text: String,
    state: Option<u8>,
    state_name: String,
    fault: u16,
    fault_name: String,
    warn: u8,
    mode: Option<u8>,
    mode_name: String,
    vbus: Option<f64>,
    pos: Option<f64>,
    vel: Option<f64>,
    torque: Option<f64>,
    current: Option<CurrentState>,
    last_rx_us: u64,
    config: CurrentConfig,
    config_label: String,
    config_pending: HashMap<u8, u8>,
    config_deadline_us: u64,
    config_retries: u8,
    current_read_pending: bool,
    info_seq: Option<u8>,
    direct_current: bool,
    cyclic: Option<Cyclic>,
    outbox: VecDeque<Frame>,
    setup: VecDeque<SetupStep>,
    setup_wait: Option<SetupWait>,
    setup_done: SetupDone,
    pending_node: Option<u8>,
    confirm_at_us: Option<u64>,
    fb_count: u32,
    fb_t0_us: u64,
    fb_hz: f64,
    status_due_us: u64,
    current_due_us: u64,
    feedback_due_us: u64,
    gains: [Option<Gains>; 3],
}

impl Device {
    fn new(now_us: u64) -> Self {
        Self {
            online: false,
            board_name: String::new(),
            fw: String::new(),
            supports_current: false,
            control_ext: false,
            supports_commission: false,
            position_warn: false,
            temp_sensor: false,
            temp: None,
            vbus_hold: None,
            temp_hold: None,
            inner: None,
            inner_text: "未知".into(),
            caps_text: String::new(),
            state: None,
            state_name: "—".into(),
            fault: 0,
            fault_name: "—".into(),
            warn: 0,
            mode: None,
            mode_name: "—".into(),
            vbus: None,
            pos: None,
            vel: None,
            torque: None,
            current: None,
            last_rx_us: now_us,
            config: CurrentConfig::default(),
            config_label: "等待支持电流协议的设备".into(),
            config_pending: HashMap::new(),
            config_deadline_us: 0,
            config_retries: 0,
            current_read_pending: false,
            info_seq: None,
            direct_current: false,
            cyclic: None,
            outbox: VecDeque::new(),
            setup: VecDeque::new(),
            setup_wait: None,
            setup_done: SetupDone::None,
            pending_node: None,
            confirm_at_us: None,
            fb_count: 0,
            fb_t0_us: now_us,
            fb_hz: 0.0,
            status_due_us: now_us.saturating_add(STATUS_PERIOD_US),
            current_due_us: now_us,
            feedback_due_us: now_us,
            gains: [None, None, None],
        }
    }

    fn reset_cache(&mut self, now_us: u64) {
        let fresh = Device::new(now_us);
        *self = fresh;
    }
}

enum ScanPhase {
    /// 探测帧还在慢队列里。
    Info { left: usize },
    /// 等最后一帧探测的应答。
    Grace { at: u64 },
    /// 状态帧还在慢队列里。
    Status { left: usize },
    /// 一台一台读电流配置。
    Config,
}

struct Scan {
    probes: HashMap<u8, u8>,
    phase: ScanPhase,
    config: VecDeque<u8>,
    config_active: Option<u8>,
}

/// 界面每帧读取的设备摘要。
#[derive(Clone, Debug)]
pub struct DeviceSnap {
    /// 节点号。
    pub id: u8,
    /// 已经应答过。
    pub online: bool,
    /// 板卡名。
    pub board_name: String,
    /// 固件版本。
    pub fw: String,
    /// 能力 bit3。
    pub supports_current: bool,
    /** @brief 能力 bit4：DISABLE、广播停止、GET_FEEDBACK、故障主动上报。 */
    pub control_ext: bool,
    /// 设备能力 bit7，上位机校准与辨识。
    pub supports_commission: bool,
    /// 内环文字。
    pub inner_text: String,
    /// 内环原始值。界面用来选中下拉框。
    pub inner: Option<u8>,
    /** @brief 能力说明。 */
    pub caps_text: String,
    /// 状态名。
    pub state_name: String,
    /** @brief 故障位，0 为无故障。 */
    pub fault: u16,
    /// 故障名。
    pub fault_name: String,
    /** @brief 位置警告位（`WARN_*`）。设备没有能力 bit5 时恒为 0。 */
    pub warn: u8,
    /** @brief 外环模式原始值，未读到状态时为空。 */
    pub mode: Option<u8>,
    /// 外环名。
    pub mode_name: String,
    /** @brief 母线电压，V。 */
    pub vbus: Option<f64>,
    /** @brief 板温，°C。设备没有能力 bit6 时为空。 */
    pub temp: Option<f64>,
    /** @brief 位置，rad。 */
    pub pos: Option<f64>,
    /** @brief 速度，rad/s。 */
    pub vel: Option<f64>,
    /** @brief 输出轴指令力矩，Nm。 */
    pub torque: Option<f64>,
    /** @brief 最近一次电流实测。 */
    pub current: Option<CurrentState>,
    /** @brief 反馈频率，Hz。 */
    pub fb_hz: f64,
    /** @brief 距最近一次上行帧的秒数。 */
    pub age_s: f64,
    /** @brief 超过 3 s 没有上行帧。 */
    pub stale: bool,
    /// 电流配置说明。
    pub config_label: String,
    /** @brief 电流配置已收齐。 */
    pub config: CurrentConfig,
    /// 可以切内环、刷新配置。
    pub current_idle: bool,
    /// 正在等设置 ACK。
    pub busy: bool,
    /// 正在循环。
    pub cyclic: bool,
    /** @brief 正在循环的帧类型，界面据此决定输入框改动是否实时下发。 */
    pub cyclic_kind: Option<CyclicKind>,
    /** @brief 循环频率，Hz。 */
    pub cyclic_hz: f64,
    /// 直接电流未归零。
    pub direct_current: bool,
    /** @brief 各模式最近读回的增益，下标为模式值。 */
    pub gains: [Option<Gains>; 3],
}

/// 示波快照。
#[derive(Clone, Debug)]
pub struct ScopeSnap {
    /// 曲线。
    pub series: Vec<PlotSeries>,
    /// 采样点数。
    pub samples: usize,
    /// 视图开关。
    pub view: ScopeView,
    /// 跟随横轴。空表示让用户缩放保留。
    pub x_range: Option<(f64, f64)>,
    /// 跟随纵轴。
    pub y_range: Option<(f64, f64)>,
}

/// 界面快照。
#[derive(Clone, Debug)]
pub struct Snapshot {
    /// 适配器已打开。
    pub connected: bool,
    /// 顶栏状态。
    pub link_text: String,
    /// 最近一秒的收发帧率。
    pub bus_fps: f64,
    /** @brief 按循环与轮询估算的总线帧率，帧/s。 */
    pub load_est: f64,
    /** @brief 估算负载超过总线约 70%。 */
    pub overload: bool,
    /** @brief 电流轮询频率，Hz，0 为关闭。 */
    pub current_hz: f64,
    /// 设备列表。
    pub devices: Vec<DeviceSnap>,
    /// 选中节点。
    pub selected: Option<u8>,
    /// 日志。
    pub logs: Vec<String>,
    /// 需要弹窗的错误。
    pub error: Option<String>,
    /// 内环切换成功后加一，界面把指令输入清零。
    pub input_reset: u64,
    /// 窗口标题。
    pub title: String,
    /// 曲线。
    pub scope: ScopeSnap,
}

impl Default for Snapshot {
    fn default() -> Self {
        Self {
            connected: false,
            link_text: "未连接".into(),
            bus_fps: 0.0,
            load_est: 0.0,
            overload: false,
            current_hz: 0.0,
            devices: Vec::new(),
            selected: None,
            logs: Vec::new(),
            error: None,
            input_reset: 0,
            title: HOST_TITLE.into(),
            scope: ScopeSnap {
                series: Vec::new(),
                samples: 0,
                view: ScopeView::default(),
                x_range: None,
                y_range: None,
            },
        }
    }
}

/// 多节点会话。
pub struct BusSession {
    devices: BTreeMap<u8, Device>,
    selected: Option<u8>,
    scope: Scope,
    seq: u8,
    paced: VecDeque<Frame>,
    pace_at_us: u64,
    scan: Option<Scan>,
    logs: VecDeque<String>,
    error: Option<String>,
    input_reset: u64,
    connected: bool,
    link_text: String,
    load_window_us: u64,
    load_frames: u32,
    bus_fps: f64,
    zero_after_rx: Vec<u8>,
    status_hz: f64,
    current_hz: f64,
    feedback_hz: f64,
}

impl Default for BusSession {
    fn default() -> Self {
        Self::new()
    }
}

impl BusSession {
    /// 空会话，还没打开适配器。
    pub fn new() -> Self {
        Self {
            devices: BTreeMap::new(),
            selected: None,
            scope: Scope::default(),
            seq: 1,
            paced: VecDeque::new(),
            pace_at_us: 0,
            scan: None,
            logs: VecDeque::new(),
            error: None,
            input_reset: 0,
            connected: false,
            link_text: "未连接".into(),
            load_window_us: 0,
            load_frames: 0,
            bus_fps: 0.0,
            zero_after_rx: Vec::new(),
            status_hz: DEFAULT_STATUS_HZ,
            current_hz: 0.0,
            feedback_hz: DEFAULT_FEEDBACK_HZ,
        }
    }

    /// 适配器刚打开。清掉上一台总线的缓存，保留轮询设置。
    pub fn connect_reset(&mut self, text: impl Into<String>) {
        let (status_hz, current_hz, feedback_hz) = (self.status_hz, self.current_hz, self.feedback_hz);
        *self = Self::new();
        self.status_hz = status_hz;
        self.current_hz = current_hz;
        self.feedback_hz = feedback_hz;
        self.connected = true;
        self.link_text = text.into();
        self.log(format!("connected {}", self.link_text));
    }

    /// 断开前停止全部节点：直接电流补 0 A，再广播 DISABLE。
    pub fn disconnect(&mut self, bus: &mut dyn CanBus, now_us: u64) {
        let _ = self.stop_all(bus, now_us);
        self.connected = false;
        self.link_text = "未连接".into();
        self.scan = None;
        self.paced.clear();
        self.log("disconnected");
    }

    /// 关掉弹窗。
    pub fn ack_error(&mut self) {
        self.error = None;
    }

    /// 适配器是否已经打开。
    pub fn is_connected(&self) -> bool {
        self.connected
    }

    /// 连接失败等不来自设备的错误。
    pub fn set_error(&mut self, text: impl Into<String>) {
        self.error = Some(text.into());
    }

    /// 执行一条界面命令。
    pub fn command(&mut self, cmd: Command, now_us: u64, bus: &mut dyn CanBus) -> Result<(), String> {
        match cmd {
            Command::Select(id) => {
                if !(1..=63).contains(&id) {
                    return Err("节点号必须是 1~63".into());
                }
                self.selected = Some(id);
                Ok(())
            }
            Command::Query(id) => self.query_device(id, now_us),
            Command::Scan => {
                let mut probes = HashMap::new();
                for id in 1..=63u8 {
                    let seq = self.next_seq();
                    probes.insert(id, seq);
                    self.paced.push_back(Frame {
                        id: ids(id).mgmt,
                        data: pack_mgmt(CMD_GET_DEVICE_INFO, seq, 0),
                    });
                }
                self.scan = Some(Scan {
                    probes,
                    phase: ScanPhase::Info { left: 63 },
                    config: VecDeque::new(),
                    config_active: None,
                });
                self.log("开始扫描节点 1~63");
                Ok(())
            }
            Command::Drive { target, cyclic } => self.drive(target, cyclic, now_us, bus),
            Command::On(id, inner) => {
                if !(1..=63).contains(&id) {
                    return Err("节点号必须是 1~63".into());
                }
                let previous = self.selected;
                self.selected = Some(id);
                let keep = matches!(*inner, Command::Select(_));
                let result = self.command(*inner, now_us, bus);
                if !keep && previous != Some(id) {
                    self.selected = previous;
                }
                result
            }
            Command::Telemetry { status_hz, current_hz, feedback_hz } => {
                self.status_hz = status_hz.clamp(0.0, 50.0);
                self.current_hz = current_hz.clamp(0.0, 500.0);
                self.feedback_hz = feedback_hz.clamp(0.0, 200.0);
                for dev in self.devices.values_mut() {
                    dev.status_due_us = now_us;
                    dev.current_due_us = now_us;
                    dev.feedback_due_us = now_us;
                }
                self.log(format!(
                    "轮询：状态 {} Hz，电流 {} Hz，空闲反馈 {} Hz",
                    fmt_g(self.status_hz),
                    fmt_g(self.current_hz),
                    fmt_g(self.feedback_hz)
                ));
                Ok(())
            }
            Command::Stop => {
                let id = self.selected.ok_or("没有选中的设备")?;
                self.stop_device(id, bus, now_us)?;
                if self.devices.get(&id).is_some_and(|dev| dev.control_ext) {
                    let seq = self.next_seq();
                    self.enqueue(id, Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_DISABLE, seq, 0) });
                    self.log(format!("ID{id} 已停止并 DISABLE 关桥"));
                } else {
                    self.log(format!("ID{id} 已停止发送；旧固件会保持最后目标"));
                }
                Ok(())
            }
            Command::StopAll => {
                self.stop_all(bus, now_us)?;
                self.log("全部停止：已广播 DISABLE");
                Ok(())
            }
            Command::Gains { mode, kp, ki, kd } => {
                let id = self.need_ready()?;
                if self.devices.get(&id).is_some_and(|dev| dev.direct_current) {
                    self.stop_device(id, bus, now_us)?;
                }
                let seq = self.next_seq();
                self.enqueue(id, Frame { id: ids(id).gains, data: pack_gains(mode, seq, kp, ki, kd) });
                self.log(format!("TX SET_GAINS ID{id} mode={} seq={seq} kp={kp} ki={ki} kd={kd}", mode_name(mode)));
                Ok(())
            }
            Command::GetGains { mode } => {
                let id = self.need_ready()?;
                let seq = self.next_seq();
                self.enqueue(id, Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_GET_GAINS, seq, mode) });
                self.log(format!("TX GET_GAINS ID{id} mode={} seq={seq}", mode_name(mode)));
                Ok(())
            }
            Command::SaveParams => {
                let id = self.need_ready()?;
                let seq = self.next_seq();
                self.enqueue(id, Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_SAVE_USER_PARAMS, seq, 0) });
                self.log(format!("TX SAVE_USER_PARAMS ID{id} seq={seq}"));
                Ok(())
            }
            Command::Mgmt { cmd } => {
                let id = self.need_ready()?;
                let query = matches!(cmd, CMD_GET_STATUS | CMD_GET_CURRENT_STATE | CMD_GET_FEEDBACK);
                if cmd == CMD_START_CALI || cmd == CMD_DISABLE {
                    self.stop_device(id, bus, now_us)?;
                } else if !query && self.devices.get(&id).is_some_and(|dev| dev.direct_current) {
                    self.stop_device(id, bus, now_us)?;
                }
                let seq = self.next_seq();
                self.enqueue(id, Frame { id: ids(id).mgmt, data: pack_mgmt(cmd, seq, 0) });
                self.log(format!("TX {} ID{id} seq={seq}", yfoc_protocol::cmd_name(cmd)));
                Ok(())
            }
            Command::SetMode { mode } => {
                let id = self.need_ready()?;
                self.stop_device(id, bus, now_us)?;
                let seq = self.next_seq();
                if let Some(dev) = self.devices.get_mut(&id) {
                    dev.mode = None;
                }
                self.enqueue(id, Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_SET_MODE, seq, mode) });
                self.log(format!("TX SET_CONTROL_MODE ID{id} {} seq={seq}", mode_name(mode)));
                Ok(())
            }
            Command::SetNodeId { new_id } => {
                if !(1..=63).contains(&new_id) {
                    return Err("新 ID 必须是 1~63".into());
                }
                let id = self.need_ready()?;
                self.stop_device(id, bus, now_us)?;
                let seq = self.next_seq();
                if let Some(dev) = self.devices.get_mut(&id) {
                    dev.pending_node = Some(new_id);
                }
                self.enqueue(id, Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_SET_NODE_ID, seq, new_id) });
                self.log(format!("TX SET_NODE_ID {id} → {new_id} seq={seq}"));
                Ok(())
            }
            Command::InnerLoop { mode } => self.begin_inner(mode, now_us, bus),
            Command::Retarget(target) => {
                if let Err(err) = self.retarget(target) {
                    self.log(format!("循环目标未更新：{err}"));
                }
                Ok(())
            }
            Command::RefreshCurrent => {
                let id = self.need_ready()?;
                if !self.devices.get(&id).is_some_and(|dev| dev.supports_current) {
                    return Err("当前设备没有电流协议".into());
                }
                self.start_config(id, now_us, false);
                Ok(())
            }
            view => {
                self.view_command(view, now_us);
                Ok(())
            }
        }
    }

    /** @brief 执行示波视图命令；其他命令忽略。 */
    pub fn view_command(&mut self, cmd: Command, now_us: u64) {
        let view = self.scope.view_mut();
        match cmd {
            Command::ScopePause(paused) => self.scope.set_paused(paused, now_us),
            Command::ScopeFollow(follow) => view.follow = follow,
            Command::ScopeClear => self.scope.clear(),
            Command::ScopeTimeAxis(time_axis) => {
                view.time_axis = time_axis;
                view.follow = true;
            }
            Command::ScopeWindowSecs(value) => view.window_s = value.clamp(0.2, 60.0),
            Command::ScopeWindowFrames(value) => view.window_frames = value.clamp(50.0, 20_000.0),
            Command::ScopeNormalize(on) => view.normalize = on,
            Command::ScopeAutoY(on) => view.auto_y = on,
            Command::ScopeChannel { index, on } => {
                if let Some(flag) = view.enabled.get_mut(index) {
                    *flag = on;
                }
            }
            Command::ScopeDevice { id, on } => {
                if on {
                    view.shown.insert(id);
                } else {
                    view.shown.remove(&id);
                }
            }
            Command::ScopeWidth(width) => view.width_px = width.clamp(64.0, 4096.0),
            Command::ScopeManualX { min, max } => {
                if max > min {
                    view.manual_x = Some((min, max));
                }
            }
            _ => {}
        }
    }

    /// 收包、超时、扫描和到期的循环发送。
    pub fn poll(&mut self, now_us: u64, bus: &mut dyn CanBus) {
        self.expire_setup(now_us);
        self.drain_rx(now_us, bus);
        let zeros: Vec<u8> = self.zero_after_rx.drain(..).collect();
        for id in zeros {
            let _ = self.stop_device(id, bus, now_us);
        }
        self.service_config_timeouts(now_us);
        self.service_confirm(now_us);
        self.service_scan(now_us);
        self.service_status_poll(now_us);
        self.flush_outbox(now_us, bus);
        self.flush_paced(now_us, bus);
        self.service_cyclic(now_us, bus);
        self.roll_load(now_us);
    }

    /// 下一次需要醒来的时间，微秒。
    pub fn next_wait_us(&self, now_us: u64) -> u64 {
        let mut soon = 1_000u64;
        for dev in self.devices.values() {
            if let Some(cyclic) = &dev.cyclic {
                soon = soon.min(cyclic.next_us.saturating_sub(now_us));
            }
        }
        if !self.paced.is_empty() {
            soon = soon.min(self.pace_at_us.saturating_sub(now_us));
        }
        soon.min(1_000)
    }

    /// 复制界面需要的状态，并按当前宽度生成曲线。
    pub fn snapshot(&self, now_us: u64) -> Snapshot {
        let devices = self.devices.iter().map(|(id, dev)| self.device_snap(*id, dev, now_us)).collect();
        let (x0, mut x1) = if self.scope.view().follow {
            self.scope.follow_x(now_us, self.connected)
        } else {
            self.scope.view().manual_x.unwrap_or_else(|| self.scope.follow_x(now_us, self.connected))
        };
        if x1 <= x0 {
            x1 = x0 + 1.0;
        }
        let series = self.scope.series(x0, x1);
        let (x_range, y_range) = if self.scope.view().follow {
            (Some((x0, x1)), self.scope.y_range(&series))
        } else {
            (None, None)
        };
        let title = self
            .selected
            .and_then(|id| self.devices.get(&id))
            .filter(|dev| !dev.board_name.is_empty())
            .map(|dev| format!("{HOST_TITLE} · {}  fw {}", dev.board_name, dev.fw))
            .unwrap_or_else(|| HOST_TITLE.into());
        Snapshot {
            connected: self.connected,
            link_text: self.link_text.clone(),
            bus_fps: self.bus_fps,
            load_est: self.load_est(),
            overload: self.overload(),
            current_hz: self.current_hz,
            devices,
            selected: self.selected,
            logs: self.logs.iter().cloned().collect(),
            error: self.error.clone(),
            input_reset: self.input_reset,
            title,
            scope: ScopeSnap {
                series,
                samples: self.scope.samples(),
                view: self.scope.view().clone(),
                x_range,
                y_range,
            },
        }
    }

    fn device_snap(&self, id: u8, dev: &Device, now_us: u64) -> DeviceSnap {
        let idle = dev.supports_current && dev.setup_wait.is_none() && dev.config_pending.is_empty();
        DeviceSnap {
            id,
            online: dev.online,
            board_name: dev.board_name.clone(),
            fw: dev.fw.clone(),
            supports_current: dev.supports_current,
            control_ext: dev.control_ext,
            supports_commission: dev.supports_commission,
            inner_text: dev.inner_text.clone(),
            inner: dev.inner,
            caps_text: dev.caps_text.clone(),
            state_name: dev.state_name.clone(),
            fault: dev.fault,
            fault_name: dev.fault_name.clone(),
            warn: dev.warn,
            mode: dev.mode,
            mode_name: dev.mode_name.clone(),
            vbus: dev.vbus,
            temp: dev.temp,
            pos: dev.pos,
            vel: dev.vel,
            torque: dev.torque,
            current: dev.current.clone(),
            fb_hz: dev.fb_hz,
            age_s: now_us.saturating_sub(dev.last_rx_us) as f64 / 1e6,
            stale: dev.online && now_us.saturating_sub(dev.last_rx_us) > STALE_US,
            config_label: dev.config_label.clone(),
            config: dev.config.clone(),
            current_idle: idle,
            busy: dev.setup_wait.is_some(),
            cyclic: dev.cyclic.is_some(),
            cyclic_kind: dev.cyclic.as_ref().map(|cyclic| cyclic.kind),
            cyclic_hz: dev.cyclic.as_ref().map_or(0.0, |cyclic| cyclic.hz),
            direct_current: dev.direct_current,
            gains: dev.gains.clone(),
        }
    }

    /**
     * @brief 估算总线帧率。
     *
     * 每条循环命令和每次轮询都对应一帧下行加一帧应答。
     */
    fn load_est(&self) -> f64 {
        let cyclic: f64 = self.devices.values().filter_map(|dev| dev.cyclic.as_ref()).map(|cyclic| cyclic.hz).sum();
        let online = self.devices.values().filter(|dev| dev.online).count() as f64;
        let current = self.devices.values().filter(|dev| dev.online && dev.supports_current).count() as f64;
        let idle = self.devices.values().filter(|dev| dev.online && dev.control_ext && dev.cyclic.is_none()).count() as f64;
        (cyclic + online * self.status_hz + current * self.current_hz + idle * self.feedback_hz) * 2.0
    }

    fn overload(&self) -> bool {
        self.load_est() > BUS_CAPACITY_HZ * 0.7
    }

    fn query_device(&mut self, id: u8, now_us: u64) -> Result<(), String> {
        if !(1..=63).contains(&id) {
            return Err("节点号必须是 1~63".into());
        }
        self.selected = Some(id);
        let info_seq = self.next_seq();
        let status_seq = self.next_seq();
        let dev = self.devices.entry(id).or_insert_with(|| Device::new(now_us));
        dev.info_seq = Some(info_seq);
        dev.status_due_us = now_us.saturating_add(STATUS_PERIOD_US);
        dev.outbox.push_back(Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_GET_DEVICE_INFO, info_seq, 0) });
        dev.outbox.push_back(Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_GET_STATUS, status_seq, 0) });
        Ok(())
    }

    fn begin_current(&mut self, amps: f64, cyclic: bool, rate: f64, now_us: u64, bus: &mut dyn CanBus) -> Result<(), String> {
        let id = self.need_ready()?;
        let ready = self.devices.get(&id).is_some_and(|dev| {
            dev.supports_current && dev.inner == Some(INNER_LOOP_CURRENT) && dev.config_pending.is_empty() && dev.config.has_scale()
        });
        if !ready {
            return Err("请先确认电流内环并读取当前设备配置".into());
        }
        let data = pack_direct_iq(amps, self.devices.get(&id).map(|dev| &dev.config))?;
        if cyclic {
            check_rate(rate)?;
        }
        self.stop_device(id, bus, now_us)?;
        let mode_seq = self.next_seq();
        let dev = self.devices.get_mut(&id).expect("device");
        dev.setup.push_back(SetupStep {
            cmd: CMD_SET_MODE,
            frame: Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_SET_MODE, mode_seq, MODE_MOTION) },
        });
        dev.setup_done = SetupDone::Current { amps, data, cyclic, rate };
        self.pump_setup(id, now_us);
        Ok(())
    }

    fn begin_inner(&mut self, mode: u8, now_us: u64, bus: &mut dyn CanBus) -> Result<(), String> {
        let id = self.need_ready()?;
        if !self.devices.get(&id).is_some_and(|dev| dev.supports_current) {
            return Err("当前设备不能切换内环".into());
        }
        self.stop_device(id, bus, now_us)?;
        let seq = self.next_seq();
        let dev = self.devices.get_mut(&id).expect("device");
        dev.setup.push_back(SetupStep {
            cmd: CMD_SET_INNER_LOOP,
            frame: Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_SET_INNER_LOOP, seq, mode) },
        });
        dev.setup_done = SetupDone::Inner;
        self.pump_setup(id, now_us);
        Ok(())
    }

    fn need_ready(&self) -> Result<u8, String> {
        let id = self.selected.ok_or("没有选中的设备")?;
        let dev = self.devices.get(&id).ok_or("没有选中的设备")?;
        if dev.setup_wait.is_some() {
            return Err("当前设置尚未确认，请等待 ACK".into());
        }
        Ok(id)
    }

    /**
     * @brief 给选中节点下发目标。
     *
     * 同类循环运行中只改写目标；外环模式已知且相符时直接发送；
     * 否则先停本节点，发 SET_CONTROL_MODE 并等 ACK，成功后再发目标。
     */
    fn drive(&mut self, target: Target, cyclic: bool, now_us: u64, bus: &mut dyn CanBus) -> Result<(), String> {
        if cyclic {
            check_rate(target.rate())?;
        }
        if let Target::Current { amps, rate } = target {
            let running = self.selected.and_then(|id| self.devices.get(&id)).and_then(|dev| dev.cyclic.as_ref()).map(|c| c.kind);
            if cyclic && running == Some(CyclicKind::Current) {
                return self.retarget(target);
            }
            return self.begin_current(amps, cyclic, rate, now_us, bus);
        }
        let id = self.need_ready()?;
        let dev = self.devices.get(&id).ok_or("没有选中的设备")?;
        let running = dev.cyclic.as_ref().map(|c| c.kind);
        if cyclic && running == Some(target.kind()) {
            return self.retarget(target);
        }
        let mode_ok = dev.mode == Some(target.mode());
        if dev.direct_current || running.is_some() || !mode_ok {
            self.stop_device(id, bus, now_us)?;
        }
        if mode_ok {
            self.send_target(id, target, cyclic, now_us);
            return Ok(());
        }
        let want = target.mode();
        let seq = self.next_seq();
        let dev = self.devices.get_mut(&id).expect("device");
        dev.setup.push_back(SetupStep {
            cmd: CMD_SET_MODE,
            frame: Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_SET_MODE, seq, want) },
        });
        dev.setup_done = SetupDone::Drive { target, cyclic };
        self.log(format!("ID{id} 自动切换外环到 {}", mode_name(want)));
        self.pump_setup(id, now_us);
        Ok(())
    }

    /** @brief 外环已就绪，发送一帧或启动循环。 */
    fn send_target(&mut self, id: u8, target: Target, cyclic: bool, now_us: u64) {
        let Some((can_id, payload, text)) = target.frame(id) else { return };
        if cyclic {
            let rate = target.rate();
            self.start_cyclic(id, target.kind(), now_us, can_id, payload, rate);
            self.log(format!("循环 ID{id} {text} {rate:.1} Hz"));
        } else {
            self.enqueue(id, Frame { id: can_id, data: payload });
            self.log(format!("TX ID{id} {text}"));
        }
    }

    fn start_cyclic(&mut self, id: u8, kind: CyclicKind, now_us: u64, can_id: u32, payload: [u8; 8], rate: f64) {
        let period_us = period_of(rate);
        if let Some(dev) = self.devices.get_mut(&id) {
            dev.cyclic = Some(Cyclic { kind, period_us, next_us: now_us, can_id, payload, hz: rate });
        }
    }

    /**
     * @brief 原地替换循环帧内容和周期，不重排下一次发送时刻，也不重走 ACK 握手。
     *
     * 直接 Iq 按设备能力和电流上限校验，校验失败时保留旧目标。
     */
    fn retarget(&mut self, target: Target) -> Result<(), String> {
        let Some(id) = self.selected else { return Ok(()) };
        let Some(dev) = self.devices.get(&id) else { return Ok(()) };
        let Some(kind) = dev.cyclic.as_ref().map(|cyclic| cyclic.kind) else { return Ok(()) };
        if kind != target.kind() {
            return Ok(());
        }
        let rate = target.rate();
        check_rate(rate)?;
        let (payload, text) = match target {
            Target::Current { amps, .. } => {
                (pack_direct_iq(amps, Some(&dev.config))?, format!("Iq {amps} A"))
            }
            other => match other.frame(id) {
                Some((_, payload, text)) => (payload, text),
                None => return Ok(()),
            },
        };
        let dev = self.devices.get_mut(&id).expect("device");
        if kind == CyclicKind::Current {
            dev.direct_current = true;
        }
        if let Some(cyclic) = dev.cyclic.as_mut() {
            cyclic.payload = payload;
            if (cyclic.hz - rate).abs() > f64::EPSILON {
                cyclic.hz = rate;
                cyclic.period_us = period_of(rate);
            }
        }
        self.log(format!("更新循环 ID{id} {text} {rate:.1} Hz"));
        Ok(())
    }

    /**
     * @brief 停止全部节点，并立即广播 DISABLE。
     *
     * 广播不排队、不等 ACK；旧固件的硬件过滤器收不到这帧，只能停在不再发送。
     */
    fn stop_all(&mut self, bus: &mut dyn CanBus, now_us: u64) -> Result<(), String> {
        let nodes: Vec<u8> = self.devices.keys().copied().collect();
        for id in nodes {
            self.stop_device(id, bus, now_us)?;
        }
        self.send_frame(bus, Frame { id: CAN_BROADCAST, data: pack_mgmt(CMD_DISABLE, 0, 0) }, now_us)
    }

    fn stop_device(&mut self, id: u8, bus: &mut dyn CanBus, now_us: u64) -> Result<(), String> {
        let Some(dev) = self.devices.get_mut(&id) else {
            return Ok(());
        };
        dev.setup.clear();
        dev.setup_wait = None;
        dev.setup_done = SetupDone::None;
        dev.cyclic = None;
        dev.outbox.retain(|frame| !is_realtime(frame.id));
        let direct = dev.direct_current;
        dev.direct_current = false;
        self.paced.retain(|frame| !(node_of(frame.id) == Some(id) && is_realtime(frame.id)));
        if direct {
            let frame = Frame { id: ids(id).iq_cmd, data: [0; 8] };
            self.send_frame(bus, frame, now_us)?;
            self.log("TX 0 A（直接电流停止）");
        }
        Ok(())
    }

    fn enqueue(&mut self, id: u8, frame: Frame) {
        if let Some(dev) = self.devices.get_mut(&id) {
            dev.outbox.push_back(frame);
        }
    }

    fn next_seq(&mut self) -> u8 {
        let seq = self.seq;
        self.seq = self.seq.wrapping_add(1);
        seq
    }

    fn log(&mut self, text: impl Into<String>) {
        self.logs.push_back(text.into());
        while self.logs.len() > 200 {
            self.logs.pop_front();
        }
    }

    fn send_frame(&mut self, bus: &mut dyn CanBus, frame: Frame, now_us: u64) -> Result<(), String> {
        bus.send(&frame).map_err(|err| err.message)?;
        self.note_frame(now_us);
        Ok(())
    }

    fn note_frame(&mut self, now_us: u64) {
        if self.load_window_us == 0 {
            self.load_window_us = now_us;
        }
        self.load_frames = self.load_frames.saturating_add(1);
        let elapsed = now_us.saturating_sub(self.load_window_us);
        if elapsed >= 1_000_000 {
            self.bus_fps = self.load_frames as f64 * 1_000_000.0 / elapsed as f64;
            self.load_frames = 0;
            self.load_window_us = now_us;
        }
    }

    fn roll_load(&mut self, now_us: u64) {
        if self.load_window_us == 0 {
            self.load_window_us = now_us;
        }
        if now_us.saturating_sub(self.load_window_us) >= 1_000_000 {
            let elapsed = now_us.saturating_sub(self.load_window_us);
            self.bus_fps = self.load_frames as f64 * 1_000_000.0 / elapsed as f64;
            self.load_frames = 0;
            self.load_window_us = now_us;
        }
    }

    fn expire_setup(&mut self, now_us: u64) {
        let ids: Vec<u8> = self.devices.keys().copied().collect();
        for id in ids {
            let expired = self.devices.get(&id).is_some_and(|dev| {
                dev.setup_wait.as_ref().is_some_and(|wait| now_us > wait.deadline_us)
            });
            if expired {
                if let Some(dev) = self.devices.get_mut(&id) {
                    dev.setup.clear();
                    dev.setup_wait = None;
                    dev.setup_done = SetupDone::None;
                }
                self.log(format!("ID{id} ACK 超时，已取消后续发送"));
            }
        }
    }

    fn drain_rx(&mut self, now_us: u64, bus: &mut dyn CanBus) {
        loop {
            match bus.recv_timeout(Duration::ZERO) {
                Ok(Some(frame)) => {
                    self.note_frame(now_us);
                    self.handle_rx(frame, now_us);
                }
                Ok(None) => break,
                Err(err) => {
                    self.log(format!("ERROR {err}"));
                    break;
                }
            }
        }
    }

    fn handle_rx(&mut self, frame: Frame, now_us: u64) {
        let Some(node) = node_of(frame.id) else { return };
        let Some(kind) = rx_kind(frame.id) else { return };
        if frame.data.len() != 8 {
            return;
        }
        match kind {
            RxKind::Ack => self.on_ack(node, frame.data, now_us),
            RxKind::Feedback => self.on_feedback(node, frame.data, now_us),
            RxKind::Status => self.on_status(node, frame.data, now_us),
            RxKind::Gains => self.on_gains(node, frame.data),
            RxKind::Current => self.on_current(node, frame.data, now_us),
            RxKind::CurrentConfig => self.on_config(node, frame.data, now_us),
            RxKind::Calibration => {
                let report = unpack_cali(&frame.data);
                self.log(format!(
                    "CALI ID{node} seq={} {} {}% stage={} err={}",
                    report.seq, report.state_name, report.progress, report.stage_name, report.error_name
                ));
            }
        }
    }

    fn on_ack(&mut self, node: u8, data: [u8; 8], now_us: u64) {
        if data[0] == CMD_GET_DEVICE_INFO && data[2] == 0 {
            self.on_info(node, data, now_us);
            return;
        }
        let ack = unpack_ack(&data);
        self.touch(node, now_us);
        if let Some(dev) = self.devices.get_mut(&node) {
            dev.state = Some(ack.state);
            dev.state_name = state_name(ack.state);
            dev.fault_name = fault_name(ack.fault);
        }
        let mut extra = String::new();
        if ack.cmd == CMD_SET_NODE_ID {
            extra = format!(" new_id={}", ack.new_id);
            if ack.result == 0 {
                let pending = self.devices.get(&node).and_then(|dev| dev.pending_node);
                let new_id = if (1..=63).contains(&ack.new_id) { ack.new_id } else { pending.unwrap_or(node) };
                if let Some(dev) = self.devices.get_mut(&node) {
                    dev.pending_node = None;
                }
                self.adopt(node, new_id, now_us);
            } else if let Some(dev) = self.devices.get_mut(&node) {
                dev.pending_node = None;
            }
        }
        self.log(format!(
            "ACK ID{node} {} seq={} {} state={} fault={extra_fault}{extra}",
            ack.cmd_name,
            ack.seq,
            ack.result_name,
            ack.state_name,
            extra_fault = ack.fault_name
        ));
        if ack.cmd == CMD_GET_DEVICE_INFO && ack.result != 0 {
            if let Some(dev) = self.devices.get_mut(&node) {
                dev.supports_current = false;
                dev.inner = None;
                dev.inner_text = "未知".into();
                dev.config_label = "旧固件：仅显示 pu，内环未知".into();
            }
        }
        let config_failed = ack.cmd == CMD_GET_CURRENT_CONFIG
            && ack.result != 0
            && self.devices.get(&node).is_some_and(|dev| dev.config_pending.values().any(|seq| *seq == ack.seq));
        if config_failed {
            self.fail_config(node, format!("读取电流配置失败：{}", result_name(ack.result)));
        }
        let manual_mode = ack.cmd == CMD_SET_MODE
            && ack.result == 0
            && !self.devices.get(&node).and_then(|dev| dev.setup_wait.as_ref()).is_some_and(|wait| wait.seq == ack.seq);
        if manual_mode {
            let seq = self.next_seq();
            self.enqueue(node, Frame { id: ids(node).mgmt, data: pack_mgmt(CMD_GET_STATUS, seq, 0) });
        }
        if let Some(dev) = self.devices.get_mut(&node) {
            dev.fault = ack.fault;
        }
        self.handle_setup_ack(node, ack.cmd, ack.seq, ack.result, now_us);
    }

    fn on_info(&mut self, node: u8, data: [u8; 8], now_us: u64) {
        let info = unpack_device_info(&data);
        let expected = self.devices.get(&node).and_then(|dev| dev.info_seq);
        let probed = self.scan.as_ref().and_then(|scan| scan.probes.get(&node).copied());
        if expected != Some(info.seq) && probed != Some(info.seq) {
            return;
        }
        self.touch(node, now_us);
        let caps = capability_text(info.encoder2, info.current, info.factory);
        if let Some(dev) = self.devices.get_mut(&node) {
            dev.info_seq = None;
            dev.online = true;
            dev.caps_text = caps.clone();
            dev.supports_current = info.current_loop;
            dev.control_ext = info.control_ext;
            dev.supports_commission = info.caps & 0x80 != 0;
            dev.position_warn = info.position_warn;
            dev.temp_sensor = info.temp_sensor;
            if !info.temp_sensor {
                dev.temp = None;
            }
            if !info.position_warn {
                dev.warn &= !WARN_ABS_UNKNOWN;
            }
            dev.board_name = info.board_name.clone();
            dev.fw = format!("{}.{}", info.fw_major, info.fw_minor);
            if !info.current_loop {
                dev.inner = None;
                dev.inner_text = "未知".into();
                dev.config_label = "旧固件：仅显示 pu，内环未知".into();
            }
        }
        self.scope.ensure_shown(node);
        if self.selected.is_none() {
            self.selected = Some(node);
        }
        self.log(format!("INFO ID{node} {} fw={}.{} {caps}", info.board_name, info.fw_major, info.fw_minor));
        if info.current_loop {
            if self.scan.is_some() {
                if let Some(scan) = self.scan.as_mut() {
                    if !scan.config.contains(&node) && scan.config_active != Some(node) {
                        scan.config.push_back(node);
                    }
                }
            } else {
                self.start_config(node, now_us, false);
            }
        }
    }

    fn on_feedback(&mut self, node: u8, data: [u8; 8], now_us: u64) {
        let fb = unpack_feedback(&data);
        self.touch(node, now_us);
        if let Some(dev) = self.devices.get_mut(&node) {
            dev.online = true;
            dev.pos = Some(fb.pos);
            dev.vel = Some(fb.vel);
            dev.torque = Some(fb.torque);
            dev.fb_count += 1;
            let elapsed = now_us.saturating_sub(dev.fb_t0_us);
            if elapsed >= 500_000 {
                dev.fb_hz = dev.fb_count as f64 * 1_000_000.0 / elapsed as f64;
                dev.fb_count = 0;
                dev.fb_t0_us = now_us;
            }
        }
        self.scope.push(node, Channel::Pos, now_us, fb.pos);
        self.scope.push(node, Channel::Vel, now_us, fb.vel);
        self.scope.push(node, Channel::Volt, now_us, fb.torque);
    }

    /**
     * @brief 按能力筛选状态帧警告位。
     *
     * 温度警告（bit0）始终保留。多圈未建立（bit4）需能力 bit5；其余原协议未实现位丢掉。
     */
    fn warn_bits(&self, node: u8, raw: u8) -> u8 {
        let mut warn = raw & WARN_TEMP;
        if self.devices.get(&node).is_some_and(|dev| dev.position_warn) {
            warn |= raw & WARN_ABS_UNKNOWN;
        }
        warn
    }

    fn on_status(&mut self, node: u8, data: [u8; 8], now_us: u64) {
        let supports = self.devices.get(&node).is_some_and(|dev| dev.supports_current);
        let status = unpack_status(&data, supports);
        self.touch(node, now_us);
        let fault = status.state == 0x03;
        let direct = self.devices.get(&node).is_some_and(|dev| dev.direct_current);
        let cyclic = self.devices.get(&node).is_some_and(|dev| dev.cyclic.is_some());
        let warn = self.warn_bits(node, status.warn);
        let changed = self.devices.get(&node).is_none_or(|dev| {
            dev.state != Some(status.state)
                || dev.fault != status.fault
                || dev.warn != warn
                || dev.mode != Some(status.mode)
                || (supports && dev.inner != status.inner_loop)
        });
        if let Some(dev) = self.devices.get_mut(&node) {
            dev.online = true;
            dev.state = Some(status.state);
            dev.state_name = status.state_name.clone();
            dev.fault = status.fault;
            dev.fault_name = status.fault_name.clone();
            dev.warn = warn;
            dev.mode = Some(status.mode);
            dev.mode_name = status.mode_name.clone();
            let shown_v = dev.vbus;
            let shown_t = dev.temp;
            let vbus = stable_reading(shown_v, &mut dev.vbus_hold, status.vbus, 3.0);
            dev.vbus = Some(vbus);
            dev.temp = if dev.temp_sensor {
                Some(stable_reading(shown_t, &mut dev.temp_hold, status.temp, 8.0))
            } else {
                dev.temp_hold = None;
                None
            };
            if supports {
                dev.inner = status.inner_loop;
                dev.inner_text = match status.inner_loop {
                    Some(0) => "电压".into(),
                    Some(1) => "电流".into(),
                    _ => "未知".into(),
                };
            }
        }
        if changed {
            self.log(format!(
                "STATUS ID{node} {} mode={} inner={} fault={} warn={} Vbus={:.2}V{}",
                status.state_name,
                status.mode_name,
                status.inner_loop_name,
                status.fault_name,
                warn_name(warn),
                status.vbus,
                if self.devices.get(&node).is_some_and(|dev| dev.temp_sensor) {
                    format!(" temp={:.1}C", status.temp)
                } else {
                    String::new()
                }
            ));
        }
        if fault && (direct || cyclic) {
            self.zero_after_rx.push(node);
            self.log(format!("ID{node} 故障 {}，已停止该节点的发送", status.fault_name));
        }
    }

    fn on_gains(&mut self, node: u8, data: [u8; 8]) {
        let gains = unpack_gains(&data);
        let line = format!(
            "GAINS ID{node} {} seq={} kp={} ki={} kd={}",
            gains.mode_name, gains.seq, fmt_g(gains.kp), fmt_g(gains.ki), fmt_g(gains.kd)
        );
        if let Some(dev) = self.devices.get_mut(&node) {
            if let Some(slot) = dev.gains.get_mut(gains.mode as usize) {
                *slot = Some(gains);
            }
        }
        self.log(line);
    }

    fn on_current(&mut self, node: u8, data: [u8; 8], now_us: u64) {
        if !self.devices.get(&node).is_some_and(|dev| dev.supports_current) {
            return;
        }
        let state = unpack_current_state(&data);
        self.touch(node, now_us);
        if let Some(dev) = self.devices.get_mut(&node) {
            dev.current = Some(state);
        }
        self.scope.push(node, Channel::Id, now_us, state.id);
        self.scope.push(node, Channel::Iq, now_us, state.iq);
        self.scope.push(node, Channel::IqRef, now_us, state.iq_ref);
        self.scope.push(node, Channel::Vq, now_us, state.vq);
    }

    fn on_config(&mut self, node: u8, data: [u8; 8], now_us: u64) {
        let page = match unpack_current_config(&data) {
            Ok(page) => page,
            Err(_) => return,
        };
        let matches = self.devices.get(&node).is_some_and(|dev| dev.config_pending.get(&page.page) == Some(&page.seq));
        if !matches {
            return;
        }
        if let Some(dev) = self.devices.get_mut(&node) {
            dev.config_pending.remove(&page.page);
            dev.config.apply_page(&page);
        }
        let done = self.devices.get(&node).is_some_and(|dev| dev.config_pending.is_empty());
        if !done {
            return;
        }
        let config = self.devices.get(&node).map(|dev| dev.config.clone()).unwrap_or_default();
        match describe_config(&config) {
            Ok(text) => {
                if let Some(dev) = self.devices.get_mut(&node) {
                    dev.config_label = text;
                    dev.config_deadline_us = 0;
                    dev.current_read_pending = true;
                }
            }
            Err(err) => {
                if let Some(dev) = self.devices.get_mut(&node) {
                    dev.config = CurrentConfig::default();
                    dev.config_label = format!("配置无效：{err}");
                    dev.current_read_pending = false;
                }
            }
        }
        self.finish_config_read(node, now_us);
        if self.scan.as_ref().and_then(|scan| scan.config_active) == Some(node) {
            if let Some(scan) = self.scan.as_mut() {
                scan.config_active = None;
            }
        }
    }

    fn finish_config_read(&mut self, node: u8, now_us: u64) {
        let pending = self.devices.get(&node).is_some_and(|dev| dev.current_read_pending && dev.config_pending.is_empty());
        if !pending {
            return;
        }
        if let Some(dev) = self.devices.get_mut(&node) {
            dev.current_read_pending = false;
            dev.status_due_us = now_us.saturating_add(STATUS_PERIOD_US);
        }
        let status_seq = self.next_seq();
        let current_seq = self.next_seq();
        self.enqueue(node, Frame { id: ids(node).mgmt, data: pack_mgmt(CMD_GET_STATUS, status_seq, 0) });
        self.enqueue(node, Frame { id: ids(node).mgmt, data: pack_mgmt(CMD_GET_CURRENT_STATE, current_seq, 0) });
    }

    fn start_config(&mut self, node: u8, now_us: u64, paced: bool) {
        let blocked = self.devices.get(&node).is_some_and(|dev| dev.setup_wait.is_some() || !dev.supports_current);
        if blocked {
            return;
        }
        if paced {
            if let Some(scan) = self.scan.as_mut() {
                if !scan.config.contains(&node) && scan.config_active != Some(node) {
                    scan.config.push_back(node);
                }
            }
            return;
        }
        self.arm_config(node, now_us, false);
    }

    fn arm_config(&mut self, node: u8, now_us: u64, into_paced: bool) {
        if let Some(dev) = self.devices.get_mut(&node) {
            dev.config = CurrentConfig::default();
            dev.config_pending.clear();
            dev.config_retries = 0;
            dev.current_read_pending = false;
            dev.config_deadline_us = now_us.saturating_add(CONFIG_TIMEOUT_US);
            dev.config_label = "正在读取配置…".into();
        }
        for page in [0u8, 1, 2] {
            let seq = self.next_seq();
            if let Some(dev) = self.devices.get_mut(&node) {
                dev.config_pending.insert(page, seq);
            }
            let frame = Frame { id: ids(node).mgmt, data: pack_mgmt(CMD_GET_CURRENT_CONFIG, seq, page) };
            if into_paced {
                self.paced.push_back(frame);
            } else if let Some(dev) = self.devices.get_mut(&node) {
                dev.outbox.push_back(frame);
            }
        }
    }

    fn fail_config(&mut self, node: u8, reason: String) {
        if let Some(dev) = self.devices.get_mut(&node) {
            dev.config = CurrentConfig::default();
            dev.config_pending.clear();
            dev.config_deadline_us = 0;
            dev.current_read_pending = false;
            dev.config_label = format!("{reason}；请点击刷新配置 / 电流重试");
        }
        self.log(reason);
        if self.scan.as_ref().and_then(|scan| scan.config_active) == Some(node) {
            if let Some(scan) = self.scan.as_mut() {
                scan.config_active = None;
            }
        }
    }

    fn service_config_timeouts(&mut self, now_us: u64) {
        let nodes: Vec<u8> = self.devices.keys().copied().collect();
        for id in nodes {
            if self.devices.get(&id).is_some_and(|dev| dev.setup_wait.is_some()) {
                continue;
            }
            let due = self.devices.get(&id).is_some_and(|dev| {
                !dev.config_pending.is_empty() && dev.config_deadline_us > 0 && now_us >= dev.config_deadline_us
            });
            if !due {
                self.finish_config_read(id, now_us);
                continue;
            }
            let retries = self.devices.get(&id).map(|dev| dev.config_retries).unwrap_or(0);
            if retries >= 1 {
                self.fail_config(id, "读取电流配置超时".into());
                continue;
            }
            let pages: Vec<u8> = self.devices.get(&id).map(|dev| dev.config_pending.keys().copied().collect()).unwrap_or_default();
            if let Some(dev) = self.devices.get_mut(&id) {
                dev.config_retries = dev.config_retries.saturating_add(1);
                dev.config_deadline_us = now_us.saturating_add(CONFIG_TIMEOUT_US);
            }
            for page in pages {
                let seq = self.next_seq();
                if let Some(dev) = self.devices.get_mut(&id) {
                    dev.config_pending.insert(page, seq);
                }
                self.enqueue(id, Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_GET_CURRENT_CONFIG, seq, page) });
            }
            self.log(format!("ID{id} 电流配置丢页，重试缺失页"));
            self.finish_config_read(id, now_us);
        }
    }

    fn handle_setup_ack(&mut self, node: u8, cmd: u8, seq: u8, result: u8, now_us: u64) {
        let waiting = self.devices.get(&node).and_then(|dev| dev.setup_wait.as_ref()).is_some_and(|wait| wait.cmd == cmd && wait.seq == seq);
        if !waiting {
            return;
        }
        if result != 0 {
            if let Some(dev) = self.devices.get_mut(&node) {
                dev.setup.clear();
                dev.setup_wait = None;
                dev.setup_done = SetupDone::None;
            }
            self.log(format!("ID{node} {} 失败（{}），已取消后续发送", yfoc_protocol::cmd_name(cmd), result_name(result)));
            return;
        }
        if let Some(dev) = self.devices.get_mut(&node) {
            dev.setup_wait = None;
        }
        self.pump_setup(node, now_us);
    }

    fn pump_setup(&mut self, id: u8, now_us: u64) {
        let action = {
            let Some(dev) = self.devices.get_mut(&id) else { return };
            if dev.setup_wait.is_some() {
                return;
            }
            if let Some(step) = dev.setup.pop_front() {
                let seq = step.frame.data[1];
                dev.setup_wait = Some(SetupWait { cmd: step.cmd, seq, deadline_us: now_us.saturating_add(SETUP_TIMEOUT_US) });
                dev.outbox.push_back(step.frame);
                Some(Err(step.cmd))
            } else {
                Some(Ok(std::mem::replace(&mut dev.setup_done, SetupDone::None)))
            }
        };
        match action {
            Some(Err(cmd)) => self.log(format!("TX {} ID{id}（等待 ACK）", yfoc_protocol::cmd_name(cmd))),
            Some(Ok(SetupDone::Drive { target, cyclic })) => {
                if let Some(dev) = self.devices.get_mut(&id) {
                    dev.mode = Some(target.mode());
                    dev.mode_name = mode_name(target.mode());
                }
                self.send_target(id, target, cyclic, now_us);
            }
            Some(Ok(SetupDone::Current { amps, data, cyclic, rate })) => {
                if let Some(dev) = self.devices.get_mut(&id) {
                    dev.mode = Some(MODE_MOTION);
                    dev.mode_name = mode_name(MODE_MOTION);
                }
                if cyclic {
                    self.start_cyclic(id, CyclicKind::Current, now_us, ids(id).iq_cmd, data, rate);
                } else {
                    self.enqueue(id, Frame { id: ids(id).iq_cmd, data });
                }
                if let Some(dev) = self.devices.get_mut(&id) {
                    dev.direct_current = cyclic || amps != 0.0;
                }
                self.log(format!("TX Iq {amps} A"));
            }
            Some(Ok(SetupDone::Inner)) => {
                self.input_reset = self.input_reset.wrapping_add(1);
                if let Some(dev) = self.devices.get_mut(&id) {
                    dev.inner = None;
                    dev.inner_text = "等待确认".into();
                }
                self.start_config(id, now_us, false);
            }
            Some(Ok(SetupDone::None)) | None => {}
        }
    }

    fn service_confirm(&mut self, now_us: u64) {
        let due: Vec<u8> = self
            .devices
            .iter()
            .filter_map(|(id, dev)| dev.confirm_at_us.filter(|at| now_us >= *at).map(|_| *id))
            .collect();
        for id in due {
            if let Some(dev) = self.devices.get_mut(&id) {
                dev.confirm_at_us = None;
            }
            let _ = self.query_device(id, now_us);
        }
    }

    fn service_scan(&mut self, now_us: u64) {
        let grace_at = match self.scan.as_ref().map(|scan| &scan.phase) {
            Some(ScanPhase::Grace { at }) => Some(*at),
            _ => None,
        };
        if let Some(at) = grace_at {
            if now_us >= at {
                let online: Vec<u8> = self
                    .devices
                    .iter()
                    .filter_map(|(id, dev)| dev.online.then_some(*id))
                    .collect();
                let left = online.len();
                for id in &online {
                    let seq = self.next_seq();
                    self.paced.push_back(Frame {
                        id: ids(*id).mgmt,
                        data: pack_mgmt(CMD_GET_STATUS, seq, 0),
                    });
                }
                if let Some(scan) = self.scan.as_mut() {
                    scan.phase = if left == 0 {
                        ScanPhase::Config
                    } else {
                        ScanPhase::Status { left }
                    };
                }
                self.log(format!("扫描探测结束，{left} 台在线"));
            }
            return;
        }
        let arm = self.scan.as_ref().is_some_and(|scan| {
            matches!(scan.phase, ScanPhase::Config) && scan.config_active.is_none()
        }) && self.paced.is_empty();
        if !arm {
            return;
        }
        let next = self.scan.as_mut().and_then(|scan| scan.config.pop_front());
        if let Some(id) = next {
            if let Some(scan) = self.scan.as_mut() {
                scan.config_active = Some(id);
            }
            self.arm_config(id, now_us, true);
        } else {
            let found = self.devices.values().filter(|dev| dev.online).count();
            self.scan = None;
            self.log(format!("扫描完成，{found} 台在线"));
        }
    }

    fn adopt(&mut self, old: u8, new_id: u8, now_us: u64) {
        if let Some(mut dev) = self.devices.remove(&old) {
            dev.reset_cache(now_us);
            dev.confirm_at_us = Some(now_us.saturating_add(CONFIRM_DELAY_US));
            self.devices.insert(new_id, dev);
        }
        self.scope.forget(old);
        if self.selected == Some(old) {
            self.selected = Some(new_id);
        }
        self.log(format!("host ID {old} → {new_id}"));
    }

    fn touch(&mut self, node: u8, now_us: u64) {
        let dev = self.devices.entry(node).or_insert_with(|| Device::new(now_us));
        dev.online = true;
        dev.last_rx_us = now_us;
    }

    fn flush_outbox(&mut self, now_us: u64, bus: &mut dyn CanBus) {
        let mut frames = Vec::new();
        for dev in self.devices.values_mut() {
            frames.extend(dev.outbox.drain(..));
        }
        for frame in frames {
            if let Err(err) = self.send_frame(bus, frame, now_us) {
                self.log(format!("ERROR {err}"));
            }
        }
    }

    fn flush_paced(&mut self, now_us: u64, bus: &mut dyn CanBus) {
        if now_us < self.pace_at_us {
            return;
        }
        let Some(frame) = self.paced.pop_front() else {
            return;
        };
        self.pace_at_us = now_us.saturating_add(PACE_US);
        if let Err(err) = self.send_frame(bus, frame, now_us) {
            self.log(format!("ERROR {err}"));
        }
        if let Some(scan) = self.scan.as_mut() {
            match &mut scan.phase {
                ScanPhase::Info { left } => {
                    *left = left.saturating_sub(1);
                    if *left == 0 {
                        scan.phase = ScanPhase::Grace { at: now_us.saturating_add(SCAN_GRACE_US) };
                    }
                }
                ScanPhase::Status { left } => {
                    *left = left.saturating_sub(1);
                    if *left == 0 {
                        scan.phase = ScanPhase::Config;
                    }
                }
                ScanPhase::Grace { .. } | ScanPhase::Config => {}
            }
        }
    }

    fn service_cyclic(&mut self, now_us: u64, bus: &mut dyn CanBus) {
        let due: Vec<(u8, u32, [u8; 8], u64)> = self
            .devices
            .iter()
            .filter_map(|(id, dev)| {
                dev.cyclic.as_ref().filter(|cyclic| now_us >= cyclic.next_us).map(|cyclic| {
                    (*id, cyclic.can_id, cyclic.payload, cyclic.period_us)
                })
            })
            .collect();
        for (id, can_id, payload, period) in due {
            let frame = Frame { id: can_id, data: payload };
            if let Err(err) = self.send_frame(bus, frame, now_us) {
                self.log(format!("ERROR {err}"));
            }
            if let Some(cyclic) = self.devices.get_mut(&id).and_then(|dev| dev.cyclic.as_mut()) {
                cyclic.next_us = cyclic.next_us.saturating_add(period);
                if cyclic.next_us < now_us {
                    cyclic.next_us = now_us.saturating_add(period);
                }
            }
        }
    }

    /**
     * @brief 按设定频率轮询在线节点的状态、电流实测和空闲反馈。
     *
     * 扫描、等 ACK、读配置期间不轮询，避免与握手帧交错。
     * 循环中的节点每条实时命令都会回反馈，不再额外请求。
     */
    fn service_status_poll(&mut self, now_us: u64) {
        if self.scan.is_some() {
            return;
        }
        let status_us = (self.status_hz > 0.0).then(|| period_of(self.status_hz));
        let current_us = (self.current_hz > 0.0).then(|| period_of(self.current_hz));
        let feedback_us = (self.feedback_hz > 0.0).then(|| period_of(self.feedback_hz));
        let mut due = Vec::new();
        for (id, dev) in &self.devices {
            if !dev.online || dev.setup_wait.is_some() || !dev.config_pending.is_empty() || dev.info_seq.is_some() {
                continue;
            }
            let status = status_us.is_some() && now_us >= dev.status_due_us;
            let current = current_us.is_some() && dev.supports_current && now_us >= dev.current_due_us;
            let feedback = feedback_us.is_some() && dev.control_ext && dev.cyclic.is_none() && now_us >= dev.feedback_due_us;
            if status || current || feedback {
                due.push((*id, status, current, feedback));
            }
        }
        for (id, status, current, feedback) in due {
            let status_seq = self.next_seq();
            let current_seq = self.next_seq();
            let feedback_seq = self.next_seq();
            let Some(dev) = self.devices.get_mut(&id) else { continue };
            if let (true, Some(period)) = (feedback, feedback_us) {
                dev.feedback_due_us = next_due(dev.feedback_due_us, period, now_us);
                dev.outbox.push_back(Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_GET_FEEDBACK, feedback_seq, 0) });
            }
            if let (true, Some(period)) = (status, status_us) {
                dev.status_due_us = next_due(dev.status_due_us, period, now_us);
                dev.outbox.push_back(Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_GET_STATUS, status_seq, 0) });
            }
            if let (true, Some(period)) = (current, current_us) {
                dev.current_due_us = next_due(dev.current_due_us, period, now_us);
                dev.outbox.push_back(Frame { id: ids(id).mgmt, data: pack_mgmt(CMD_GET_CURRENT_STATE, current_seq, 0) });
            }
        }
    }
}

/**
 * @brief  丢掉偏离当前显示太远、且下一帧没有重复的采样。
 *
 * 母线和 NTC 共用一次 ADC 转换，偶发会把对方的电压报上来。这种点只出现一次；
 * 真实变化会在下一帧附近再次出现。
 */
fn stable_reading(shown: Option<f64>, hold: &mut Option<f64>, sample: f64, max_step: f64) -> f64 {
    let Some(shown) = shown else {
        *hold = None;
        return sample;
    };
    if (sample - shown).abs() <= max_step {
        *hold = None;
        return sample;
    }
    if hold.is_some_and(|pending| (sample - pending).abs() <= max_step) {
        *hold = None;
        return sample;
    }
    *hold = Some(sample);
    shown
}

/** @brief 下一次轮询时刻；落后超过一个周期时从现在重新计时。 */
fn next_due(due_us: u64, period_us: u64, now_us: u64) -> u64 {
    let next = due_us.saturating_add(period_us);
    if next < now_us { now_us.saturating_add(period_us) } else { next }
}

fn period_of(rate: f64) -> u64 {
    ((1_000_000.0 / rate).round() as u64).max(1)
}

fn check_rate(rate: f64) -> Result<(), String> {
    if rate > 0.0 && rate <= 2_000.0 {
        Ok(())
    } else {
        Err("循环频率须为 0~2000 Hz".into())
    }
}

fn capability_text(encoder2: bool, current: bool, factory: bool) -> String {
    let mut caps = Vec::new();
    if encoder2 {
        caps.push("双编码器");
    }
    if current {
        caps.push("电流采样");
    }
    if factory {
        caps.push("工厂测试");
    }
    if caps.is_empty() { "无扩展能力".into() } else { caps.join("、") }
}

fn describe_config(config: &CurrentConfig) -> Result<String, String> {
    let scale = current_scale(Some(config))?;
    let i_limit = config.i_limit.ok_or("缺少电流限幅")?;
    let v_limit = config.v_limit.ok_or("缺少标幺限幅")?;
    let bandwidth = config.bandwidth_hz.ok_or("缺少带宽")?;
    let kp = config.kp.ok_or("缺少 Kp")?;
    let ki = config.ki.ok_or("缺少 Ki")?;
    let loop_hz = config.loop_hz.ok_or("缺少环频率")?;
    Ok(format!(
        "±{} A / ±{} pu = {} A/pu；带宽 {bandwidth} Hz，PI Kp={} V/A，Ki={} V/(A·s)，环频率 {loop_hz} Hz",
        fmt_g(i_limit), fmt_g(v_limit), fmt_g(scale), fmt_g(kp), fmt_g(ki)
    ))
}

fn fmt_g(value: f64) -> String {
    let text = format!("{value:.6}");
    let trimmed = text.trim_end_matches('0').trim_end_matches('.');
    if trimmed.is_empty() { "0".into() } else { trimmed.into() }
}

/// 这些命令码留给界面按钮，避免界面再依赖协议常量的全部集合。
pub mod cmds {
    pub use yfoc_protocol::{
        CMD_CLEAR_FAULT, CMD_DISABLE, CMD_GET_CURRENT_STATE, CMD_GET_FEEDBACK, CMD_GET_STATUS, CMD_SET_ZERO,
        CMD_START_CALI,
        INNER_LOOP_CURRENT, INNER_LOOP_VOLTAGE, MODE_MOTION, MODE_POSITION, MODE_VELOCITY,
    };
}

#[allow(dead_code)]
fn _limit() -> f64 {
    HOST_CURRENT_CAP_A
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;
    use yfoc_can::CanError;
    use yfoc_protocol::{
        CAP_CONTROL_EXT, CAP_POSITION_WARN, CMD_GET_CURRENT_CONFIG, CMD_GET_DEVICE_INFO,
        CMD_SET_INNER_LOOP, CMD_SET_MODE, WARN_ABS_UNKNOWN, WARN_TEMP,
    };

    struct Mock {
        sent: Vec<Frame>,
        inbox: VecDeque<Frame>,
    }

    impl CanBus for Mock {
        fn send(&mut self, frame: &Frame) -> Result<(), CanError> {
            self.sent.push(*frame);
            Ok(())
        }
        fn recv_timeout(&mut self, _: Duration) -> Result<Option<Frame>, CanError> {
            Ok(self.inbox.pop_front())
        }
        fn shutdown(&mut self) -> Result<(), CanError> {
            Ok(())
        }
    }

    fn fresh() -> (BusSession, Mock) {
        let mut session = BusSession::new();
        session.connect_reset("test");
        (session, Mock { sent: Vec::new(), inbox: VecDeque::new() })
    }

    fn info_frame(node: u8, seq: u8, caps: u8) -> Frame {
        Frame { id: ids(node).ack, data: [CMD_GET_DEVICE_INFO, seq, 0, 2, 1, caps, 1, 2] }
    }

    fn status_frame(node: u8, byte7: u8) -> Frame {
        let mut data = [0u8; 8];
        data[0] = 2;
        data[3..5].copy_from_slice(&2400u16.to_le_bytes());
        data[7] = byte7;
        Frame { id: ids(node).status, data }
    }

    fn config_frame(node: u8, page: u8, seq: u8, a: u16, b: u16, c: u16) -> Frame {
        let mut data = [0u8; 8];
        data[0] = page;
        data[1] = seq;
        data[2..4].copy_from_slice(&a.to_le_bytes());
        data[4..6].copy_from_slice(&b.to_le_bytes());
        data[6..8].copy_from_slice(&c.to_le_bytes());
        Frame { id: ids(node).current_config, data }
    }

    fn cur(amps: f64, cyclic: bool) -> Command {
        Command::Drive { target: Target::Current { amps, rate: 200.0 }, cyclic }
    }

    fn vel(vel: f64, rate: f64) -> Command {
        Command::Drive { target: Target::Velocity { vel, rate }, cyclic: true }
    }

    fn ack(node: u8, cmd: u8, seq: u8, result: u8) -> Frame {
        Frame { id: ids(node).ack, data: [cmd, seq, result, 2, 0, 0, 0, 0] }
    }

    fn negotiate(session: &mut BusSession, bus: &mut Mock, now: u64) {
        session.command(Command::Query(1), now, bus).unwrap();
        session.poll(now, bus);
        let info_seq = bus.sent.iter().find(|f| f.data[0] == CMD_GET_DEVICE_INFO).unwrap().data[1];
        bus.inbox.push_back(info_frame(1, info_seq, 11));
        bus.inbox.push_back(status_frame(1, 128));
        session.poll(now + 1, bus);
        let pages: Vec<(u8, u8)> = bus.sent.iter().filter(|f| f.data[0] == CMD_GET_CURRENT_CONFIG).map(|f| (f.data[2], f.data[1])).collect();
        for (page, seq) in pages {
            let values = match page {
                0 => (5000, 5000, 500),
                1 => (7477, 16211, 20000),
                _ => (230, 800, 1),
            };
            bus.inbox.push_back(config_frame(1, page, seq, values.0, values.1, values.2));
        }
        session.poll(now + 2, bus);
    }

    #[test]
    fn direct_iq_waits_for_mode_ack_and_preserves_motion_gains() {
        let (mut session, mut bus) = fresh();
        negotiate(&mut session, &mut bus, 0);
        assert!(session.devices[&1].config.i_limit.is_some());
        bus.sent.clear();
        session.command(cur(2.0, false), 10, &mut bus).unwrap();
        session.poll(10, &mut bus);
        assert_eq!(bus.sent.len(), 1);
        assert_eq!(bus.sent[0].data[0], CMD_SET_MODE);
        let seq = bus.sent[0].data[1];
        bus.inbox.push_back(ack(1, CMD_SET_MODE, seq.wrapping_add(1), 0));
        session.poll(11, &mut bus);
        assert!(bus.sent.iter().all(|f| f.id != ids(1).gains));
        bus.inbox.push_back(ack(1, CMD_SET_MODE, seq, 0));
        session.poll(12, &mut bus);
        assert!(bus.sent.iter().all(|f| f.id != ids(1).gains));
        let iq = bus.sent.iter().find(|f| f.id == ids(1).iq_cmd).unwrap();
        assert_eq!(iq.data, [0xd0, 0x07, 0, 0, 0, 0, 0, 0]);
        assert!(bus.sent.iter().all(|f| f.id != ids(1).motion));

        let (mut session, mut bus) = fresh();
        negotiate(&mut session, &mut bus, 0);
        bus.sent.clear();
        session.command(cur(1.0, false), 20, &mut bus).unwrap();
        session.poll(20, &mut bus);
        let seq = bus.sent[0].data[1];
        bus.inbox.push_back(ack(1, CMD_SET_MODE, seq, 3));
        session.poll(21, &mut bus);
        assert!(bus.sent.iter().all(|f| !is_realtime(f.id)));
    }

    #[test]
    fn missing_direct_iq_capability_blocks_current_before_setup() {
        let (mut session, mut bus) = fresh();
        negotiate(&mut session, &mut bus, 0);
        for capability in [Some(false), None] {
            session.devices.get_mut(&1).unwrap().config.direct_iq = capability;
            bus.sent.clear();
            assert!(session.command(cur(0.0, false), 10, &mut bus).is_err());
            session.poll(10, &mut bus);
            assert!(bus.sent.is_empty());
        }
    }

    #[test]
    fn node_switch_discards_config_and_legacy_skips_current() {
        let (mut session, mut bus) = fresh();
        negotiate(&mut session, &mut bus, 0);
        session.command(Command::SetNodeId { new_id: 2 }, 30, &mut bus).unwrap();
        session.poll(30, &mut bus);
        let seq = bus.sent.iter().rev().find(|f| f.data[0] == CMD_SET_NODE_ID).unwrap().data[1];
        let mut reply = ack(1, CMD_SET_NODE_ID, seq, 0);
        reply.data[6] = 2;
        bus.inbox.push_back(reply);
        session.poll(31, &mut bus);
        assert!(session.devices.get(&1).is_none());
        assert!(session.devices[&2].config.i_limit.is_none());
        assert!(session.devices[&2].inner.is_none());

        let (mut session, mut bus) = fresh();
        session.command(Command::Query(1), 0, &mut bus).unwrap();
        session.poll(0, &mut bus);
        let seq = bus.sent[0].data[1];
        bus.inbox.push_back(ack(1, CMD_GET_DEVICE_INFO, seq, 1));
        bus.inbox.push_back(status_frame(1, 0));
        session.poll(1, &mut bus);
        assert!(session.devices[&1].inner.is_none());
        assert!(!session.devices[&1].current_ready_like());
        assert!(bus.sent.iter().all(|f| f.data[0] != CMD_GET_CURRENT_CONFIG));
    }

    #[test]
    fn stop_and_disconnect_zero_only_direct_current() {
        let (mut session, mut bus) = fresh();
        negotiate(&mut session, &mut bus, 0);
        session.command(cur(0.0, true), 10, &mut bus).unwrap();
        session.poll(10, &mut bus);
        let mode_seq = bus.sent.iter().rev().find(|f| f.data[0] == CMD_SET_MODE).unwrap().data[1];
        bus.inbox.push_back(ack(1, CMD_SET_MODE, mode_seq, 0));
        session.poll(11, &mut bus);
        session.poll(12, &mut bus);
        assert!(session.devices[&1].direct_current);
        session.command(Command::Retarget(Target::Current { amps: -0.125, rate: 200.0 }), 12, &mut bus).unwrap();
        let cycle = session.devices[&1].cyclic.as_ref().unwrap();
        assert_eq!((cycle.can_id, cycle.payload), (ids(1).iq_cmd, [0x83, 0xff, 0, 0, 0, 0, 0, 0]));
        bus.sent.clear();
        session.command(Command::Stop, 13, &mut bus).unwrap();
        assert_eq!(bus.sent.last().unwrap(), &Frame { id: ids(1).iq_cmd, data: [0; 8] });
        assert!(!session.devices[&1].direct_current);

        session.command(Command::Select(2), 14, &mut bus).unwrap();
        session.command(Command::Query(2), 14, &mut bus).unwrap();
        bus.inbox.push_back(status_frame(2, 0x20));
        session.poll(14, &mut bus);
        session.command(Command::Select(2), 15, &mut bus).unwrap();
        session.command(vel(1.0, 100.0), 15, &mut bus).unwrap();
        session.command(Command::Select(1), 16, &mut bus).unwrap();
        session.command(cur(1.0, false), 16, &mut bus).unwrap();
        session.poll(16, &mut bus);
        let mode_seq = bus.sent.iter().rev().find(|f| f.id == ids(1).mgmt && f.data[0] == CMD_SET_MODE).unwrap().data[1];
        bus.inbox.push_back(ack(1, CMD_SET_MODE, mode_seq, 0));
        session.poll(17, &mut bus);
        session.poll(18, &mut bus);
        assert!(session.devices[&1].direct_current);
        assert!(session.devices[&2].cyclic.is_some());
        bus.sent.clear();
        session.disconnect(&mut bus, 19);
        assert!(bus.sent.iter().any(|f| f.id == ids(1).iq_cmd && f.data == [0; 8]));
        assert!(bus.sent.iter().all(|f| f.id != ids(2).vel));
    }

    #[test]
    fn inner_switch_zeros_first_and_setup_timeout_cancels() {
        let (mut session, mut bus) = fresh();
        negotiate(&mut session, &mut bus, 0);
        session.command(cur(1.0, true), 10, &mut bus).unwrap();
        session.poll(10, &mut bus);
        let mode_seq = bus.sent.iter().find(|f| f.data[0] == CMD_SET_MODE).unwrap().data[1];
        bus.inbox.push_back(ack(1, CMD_SET_MODE, mode_seq, 0));
        session.poll(11, &mut bus);
        session.poll(12, &mut bus);
        bus.sent.clear();
        session.command(Command::InnerLoop { mode: 0 }, 20, &mut bus).unwrap();
        assert_eq!(bus.sent[0], Frame { id: ids(1).iq_cmd, data: [0; 8] });
        session.poll(20, &mut bus);
        assert!(bus.sent.iter().any(|f| f.data[0] == CMD_SET_INNER_LOOP));
        assert!(session.devices[&1].cyclic.is_none());

        let (mut session, mut bus) = fresh();
        negotiate(&mut session, &mut bus, 0);
        session.command(cur(1.0, false), 10, &mut bus).unwrap();
        session.poll(10, &mut bus);
        let seq = bus.sent.last().unwrap().data[1];
        let cmd = bus.sent.last().unwrap().data[0];
        session.poll(10 + SETUP_TIMEOUT_US + 1, &mut bus);
        bus.inbox.push_back(ack(1, cmd, seq, 0));
        session.poll(10 + SETUP_TIMEOUT_US + 2, &mut bus);
        assert!(bus.sent.iter().all(|f| !is_realtime(f.id) || f.data == [0; 8]));
    }

    #[test]
    fn config_retry_is_bounded() {
        let (mut session, mut bus) = fresh();
        negotiate(&mut session, &mut bus, 0);
        bus.sent.clear();
        session.command(Command::RefreshCurrent, 100, &mut bus).unwrap();
        session.poll(100, &mut bus);
        assert_eq!(bus.sent.iter().filter(|f| f.data[0] == CMD_GET_CURRENT_CONFIG).count(), 3);
        session.poll(100 + CONFIG_TIMEOUT_US, &mut bus);
        assert_eq!(bus.sent.iter().filter(|f| f.data[0] == CMD_GET_CURRENT_CONFIG).count(), 6);
        session.poll(100 + 2 * CONFIG_TIMEOUT_US, &mut bus);
        assert!(session.devices[&1].config_pending.is_empty());
        assert!(session.devices[&1].config.i_limit.is_none());
        assert!(session.devices[&1].config_label.contains("超时"));
    }

    #[test]
    fn scan_paces_node_probes() {
        let (mut session, mut bus) = fresh();
        session.command(Command::Scan, 0, &mut bus).unwrap();
        session.poll(0, &mut bus);
        assert_eq!(bus.sent.len(), 1);
        assert_eq!(bus.sent[0].id, ids(1).mgmt);
        session.poll(1_000, &mut bus);
        assert_eq!(bus.sent.len(), 1);
        session.poll(2_000, &mut bus);
        assert_eq!(bus.sent.len(), 2);
        assert_eq!(bus.sent[1].id, ids(2).mgmt);
    }

    #[test]
    fn overload_warns_without_dropping() {
        let (mut session, mut bus) = fresh();
        session.command(Command::Query(1), 0, &mut bus).unwrap();
        session.command(Command::Query(2), 0, &mut bus).unwrap();
        bus.inbox.push_back(status_frame(1, 0));
        bus.inbox.push_back(status_frame(2, 0x20));
        session.poll(0, &mut bus);
        session.command(Command::Select(1), 1, &mut bus).unwrap();
        let motion = Target::Motion { pos: 0.0, vel: 0.0, ff: 0.0, rate: 2000.0 };
        session.command(Command::Drive { target: motion, cyclic: true }, 1, &mut bus).unwrap();
        session.command(Command::Select(2), 1, &mut bus).unwrap();
        session.command(vel(1.0, 2000.0), 1, &mut bus).unwrap();
        assert!(session.overload());
        session.poll(1, &mut bus);
        let before = bus.sent.len();
        session.poll(1 + 500, &mut bus);
        assert!(bus.sent.len() >= before);
    }

    #[test]
    fn retarget_updates_running_cycle_in_place() {
        let (mut session, mut bus) = fresh();
        session.command(Command::Query(1), 0, &mut bus).unwrap();
        bus.inbox.push_back(status_frame(1, 0x20));
        session.poll(0, &mut bus);
        session.command(vel(1.0, 100.0), 0, &mut bus).unwrap();
        session.poll(0, &mut bus);
        let next = session.devices[&1].cyclic.as_ref().unwrap().next_us;
        session.command(vel(2.0, 100.0), 1, &mut bus).unwrap();
        assert_eq!(session.devices[&1].cyclic.as_ref().unwrap().next_us, next);
        session.command(Command::Retarget(Target::Velocity { vel: 3.0, rate: 100.0 }), 1, &mut bus).unwrap();
        session.command(Command::Retarget(Target::Position { pos: 9.0, max_vel: 1.0, rate: 100.0 }), 1, &mut bus).unwrap();
        let cyclic = session.devices[&1].cyclic.as_ref().unwrap();
        assert_eq!(cyclic.payload, pack_velocity(3.0));
        assert_eq!(cyclic.next_us, next);
        bus.sent.clear();
        session.poll(next, &mut bus);
        assert!(bus.sent.iter().any(|f| f.id == ids(1).vel && f.data == pack_velocity(3.0)));
    }

    #[test]
    fn current_sample_does_not_invent_position() {
        let (mut session, mut bus) = fresh();
        negotiate(&mut session, &mut bus, 0);
        let mut data = [0u8; 8];
        data[0..2].copy_from_slice(&1000i16.to_le_bytes());
        bus.inbox.push_back(Frame { id: ids(1).current, data });
        session.poll(50, &mut bus);
        assert!(session.scope.has_channel(1, crate::scope::Channel::Id));
        assert!(!session.scope.has_channel(1, crate::scope::Channel::Pos));
    }

    #[test]
    fn drive_switches_outer_mode_before_sending() {
        let (mut session, mut bus) = fresh();
        session.command(Command::Query(1), 0, &mut bus).unwrap();
        bus.inbox.push_back(status_frame(1, 0));
        session.poll(0, &mut bus);
        bus.sent.clear();
        let target = Target::Position { pos: 1.0, max_vel: 2.0, rate: 100.0 };
        session.command(Command::Drive { target, cyclic: false }, 10, &mut bus).unwrap();
        session.poll(10, &mut bus);
        assert!(bus.sent.iter().all(|f| f.id != ids(1).pos));
        let mode = bus.sent.iter().find(|f| f.data[0] == CMD_SET_MODE).unwrap();
        assert_eq!(mode.data[2], MODE_POSITION);
        bus.inbox.push_back(ack(1, CMD_SET_MODE, mode.data[1], 0));
        session.poll(11, &mut bus);
        assert!(bus.sent.iter().any(|f| f.id == ids(1).pos && f.data == pack_position(1.0, 2.0)));
        assert_eq!(session.devices[&1].mode, Some(MODE_POSITION));
        bus.sent.clear();
        session.command(Command::Drive { target, cyclic: false }, 12, &mut bus).unwrap();
        session.poll(12, &mut bus);
        assert!(bus.sent.iter().all(|f| f.data[0] != CMD_SET_MODE || f.id != ids(1).mgmt));
        assert!(bus.sent.iter().any(|f| f.id == ids(1).pos));
    }

    #[test]
    fn on_targets_node_without_changing_selection() {
        let (mut session, mut bus) = fresh();
        for id in [1u8, 2] {
            session.command(Command::Query(id), 0, &mut bus).unwrap();
            bus.inbox.push_back(status_frame(id, 0x20));
        }
        session.poll(0, &mut bus);
        session.command(Command::Select(1), 1, &mut bus).unwrap();
        session.command(Command::On(2, Box::new(vel(1.0, 100.0))), 1, &mut bus).unwrap();
        assert_eq!(session.selected, Some(1));
        assert!(session.devices[&2].cyclic.is_some());
        assert!(session.devices[&1].cyclic.is_none());
        session.command(Command::On(2, Box::new(Command::Stop)), 2, &mut bus).unwrap();
        assert!(session.devices[&2].cyclic.is_none());
    }

    #[test]
    fn telemetry_polls_status_and_current_at_rate() {
        let (mut session, mut bus) = fresh();
        negotiate(&mut session, &mut bus, 0);
        session.command(Command::Telemetry { status_hz: 10.0, current_hz: 100.0, feedback_hz: 0.0 }, 1_000, &mut bus).unwrap();
        bus.sent.clear();
        for step in 0..100u64 {
            session.poll(1_000 + step * 1_000, &mut bus);
        }
        let status = bus.sent.iter().filter(|f| f.data[0] == CMD_GET_STATUS).count();
        let current = bus.sent.iter().filter(|f| f.data[0] == CMD_GET_CURRENT_STATE).count();
        assert_eq!(status, 1);
        assert_eq!(current, 10);
        session.command(Command::Telemetry { status_hz: 0.0, current_hz: 0.0, feedback_hz: 0.0 }, 200_000, &mut bus).unwrap();
        bus.sent.clear();
        session.poll(5_000_000, &mut bus);
        assert!(bus.sent.is_empty());
    }

    fn ext_device(session: &mut BusSession, bus: &mut Mock, node: u8, byte7: u8) {
        session.command(Command::Query(node), 0, bus).unwrap();
        session.poll(0, bus);
        let seq = bus.sent.iter().rev().find(|f| f.id == ids(node).mgmt && f.data[0] == CMD_GET_DEVICE_INFO).unwrap().data[1];
        bus.inbox.push_back(info_frame(node, seq, CAP_CONTROL_EXT));
        bus.inbox.push_back(status_frame(node, byte7));
        session.poll(1, bus);
    }

    #[test]
    fn stop_disables_and_stop_all_broadcasts() {
        let (mut session, mut bus) = fresh();
        ext_device(&mut session, &mut bus, 1, 0x20);
        session.command(vel(1.0, 100.0), 2, &mut bus).unwrap();
        session.poll(2, &mut bus);
        bus.sent.clear();
        session.command(Command::Stop, 3, &mut bus).unwrap();
        session.poll(3, &mut bus);
        assert!(session.devices[&1].cyclic.is_none());
        assert!(bus.sent.iter().any(|f| f.id == ids(1).mgmt && f.data[0] == CMD_DISABLE));
        bus.sent.clear();
        session.command(Command::StopAll, 4, &mut bus).unwrap();
        assert!(bus.sent.iter().any(|f| f.id == CAN_BROADCAST && f.data == pack_mgmt(CMD_DISABLE, 0, 0)));
    }

    #[test]
    fn idle_ext_device_polls_feedback_and_fault_stops_cycle() {
        let (mut session, mut bus) = fresh();
        ext_device(&mut session, &mut bus, 1, 0x20);
        session.command(Command::Telemetry { status_hz: 0.0, current_hz: 0.0, feedback_hz: 10.0 }, 10, &mut bus).unwrap();
        bus.sent.clear();
        for step in 0..100u64 {
            session.poll(10 + step * 1_000, &mut bus);
        }
        assert_eq!(bus.sent.iter().filter(|f| f.data[0] == CMD_GET_FEEDBACK).count(), 1);

        session.command(vel(1.0, 100.0), 200_000, &mut bus).unwrap();
        bus.sent.clear();
        session.poll(400_000, &mut bus);
        assert!(bus.sent.iter().all(|f| f.data[0] != CMD_GET_FEEDBACK || f.id != ids(1).mgmt));
        let mut fault = status_frame(1, 0x20);
        fault.data[0] = 0x03;
        fault.data[1] = 0x02;
        bus.inbox.push_back(fault);
        session.poll(400_001, &mut bus);
        assert!(session.devices[&1].cyclic.is_none());
        assert_eq!(session.devices[&1].fault, 0x02);
    }

    #[test]
    fn one_bad_sample_does_not_move_the_reading() {
        let mut hold = None;
        assert!((stable_reading(None, &mut hold, 24.0, 3.0) - 24.0).abs() < 1e-9);
        assert!((stable_reading(Some(24.0), &mut hold, 51.0, 3.0) - 24.0).abs() < 1e-9);
        assert!((stable_reading(Some(24.0), &mut hold, 23.8, 3.0) - 23.8).abs() < 1e-9);
        assert!((stable_reading(Some(23.8), &mut hold, 0.0, 3.0) - 23.8).abs() < 1e-9);
        assert!((stable_reading(Some(23.8), &mut hold, 0.1, 3.0) - 0.1).abs() < 1e-9);
    }

    /** 多圈未建立只对带能力 bit5 的设备解释；温度警告始终保留。 */
    #[test]
    fn warning_bits_need_capability() {
        let (mut session, mut bus) = fresh();
        ext_device(&mut session, &mut bus, 1, 0x20 | WARN_ABS_UNKNOWN | WARN_TEMP);
        assert_eq!(session.devices[&1].warn, WARN_TEMP);

        session.command(Command::Query(2), 10, &mut bus).unwrap();
        session.poll(10, &mut bus);
        let seq = bus.sent.iter().rev().find(|f| f.id == ids(2).mgmt && f.data[0] == CMD_GET_DEVICE_INFO).unwrap().data[1];
        bus.inbox.push_back(info_frame(2, seq, CAP_CONTROL_EXT | CAP_POSITION_WARN));
        bus.inbox.push_back(status_frame(2, 0x20 | WARN_ABS_UNKNOWN | WARN_TEMP | 0x02));
        session.poll(11, &mut bus);
        assert_eq!(session.devices[&2].warn, WARN_ABS_UNKNOWN | WARN_TEMP);
        assert_eq!(session.devices[&2].mode, Some(1));
        bus.inbox.push_back(status_frame(2, 0x20));
        session.poll(12, &mut bus);
        assert_eq!(session.devices[&2].warn, 0);
    }

    impl Device {
        fn current_ready_like(&self) -> bool {
            self.supports_current && self.inner == Some(INNER_LOOP_CURRENT)
        }
    }
}
