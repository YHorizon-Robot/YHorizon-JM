//! 单节点 CAN 客户端。

use std::time::{Duration, Instant};

use yfoc_can::{CanBus, Frame};
use yfoc_protocol::{
    current_scale, ids, pack_gains, pack_mgmt, pack_direct_iq, unpack_ack, unpack_current_config,
    unpack_current_state, unpack_device_info, unpack_feedback, unpack_status, CurrentConfig,
    DeviceInfo, Ids, Status, CMD_GET_CURRENT_CONFIG, CMD_GET_CURRENT_STATE, CMD_GET_DEVICE_INFO,
    CMD_GET_STATUS, CMD_SET_GAINS, CMD_SET_INNER_LOOP, CMD_SET_MODE, INNER_LOOP_CURRENT,
    INNER_LOOP_VOLTAGE, MODE_MOTION,
};

/// 单轴外环模式。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ControlMode {
    Motion = MODE_MOTION,
    Velocity = yfoc_protocol::MODE_VELOCITY,
    Position = yfoc_protocol::MODE_POSITION,
}

/// 一个节点及其 CAN 连接。普通实时目标在固件上持续有效，结束驱动时调用 [`Self::disable`]。
pub struct Joint<B: CanBus> {
    bus: B,
    /// 节点帧 ID。
    ids: Ids,
    seq: u8,
    /// 已确认的电流协议能力。
    supports_current: bool,
    config: Option<CurrentConfig>,
    /** @brief 已确认的位置警告能力（bit5）。未确认时不把 Byte7 bit4 当作多圈未建立。 */
    position_warn: bool,
    /** @brief 已确认焊了 NTC（能力 bit6）。未确认时温度字段按无效处理。 */
    temp_sensor: bool,
}

impl<B: CanBus> Joint<B> {
    /// 绑定已经打开的总线。只接受节点 1～63，此步骤不发送 CAN 帧。
    pub fn new(bus: B, node_id: u8) -> Result<Self, String> {
        if !(1..=63).contains(&node_id) {
            return Err(format!("无效节点 {node_id}，应为 1～63"));
        }
        Ok(Self {
            bus,
            ids: ids(node_id),
            seq: 0,
            supports_current: false,
            config: None,
            position_warn: false,
            temp_sensor: false,
        })
    }

    /// 本节点使用的标准 CAN ID。
    pub fn ids(&self) -> Ids {
        self.ids
    }

    pub(crate) fn select_node(&mut self, node: u8) {
        self.ids = ids(node);
        self.supports_current = false;
        self.config = None;
        self.temp_sensor = false;
        self.position_warn = false;
    }

    pub(crate) fn bus_mut(&mut self) -> &mut B {
        &mut self.bus
    }

    /// 最近一次能力探测确认了电流协议。
    pub fn supports_current(&self) -> bool {
        self.supports_current
    }

    /**
     * @brief 按已确认的能力解析一帧状态。
     *
     * 内环位只在电流协议确认后解释。温度警告始终保留；多圈未建立警告需能力 bit5。
     * 无 NTC 时温度读数清零。
     */
    pub fn parse_status(&self, data: &[u8; 8]) -> Status {
        let mut status = unpack_status(data, self.supports_current);
        let mut warn = status.warn & yfoc_protocol::WARN_TEMP;
        if self.position_warn {
            warn |= status.warn & yfoc_protocol::WARN_ABS_UNKNOWN;
        }
        status.warn = warn;
        if !self.temp_sensor {
            status.temp = 0.0;
        }
        status
    }

    /// 取出总线，便于最后关闭。
    pub fn into_bus(self) -> B {
        self.bus
    }

    /// 发送一帧。
    pub fn send(&mut self, can_id: u32, data: [u8; 8]) -> Result<(), String> {
        self.bus
            .send(&Frame { id: can_id, data })
            .map_err(|err| err.message)
    }

    fn next_seq(&mut self) -> u8 {
        self.seq = self.seq.wrapping_add(1);
        self.seq
    }

    fn receive<F>(&mut self, mut pred: F, timeout: Duration) -> Result<Option<[u8; 8]>, String>
    where
        F: FnMut(&Frame) -> bool,
    {
        let deadline = Instant::now() + timeout;
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            if let Some(frame) = self.recv(left.min(Duration::from_millis(20)))? {
                if pred(&frame) {
                    return Ok(Some(frame.data));
                }
            }
            if Instant::now() >= deadline {
                return Ok(None);
            }
        }
    }

    /// 发送管理命令并等待对应应答。`reply_id` 为 `None` 时只认 ACK。
    pub fn mgmt(
        &mut self,
        cmd: u8,
        arg: u8,
        reply_id: Option<u32>,
        page: Option<u8>,
    ) -> Result<[u8; 8], String> {
        let seq = self.next_seq();
        self.send(self.ids.mgmt, pack_mgmt(cmd, seq, arg))?;
        let ack_id = self.ids.ack;
        let mut answer_is_ack = false;
        let data = self
            .receive(
                |frame| {
                    if frame.id == ack_id {
                        answer_is_ack = frame.data[0] == cmd && frame.data[1] == seq;
                        return answer_is_ack
                            && (reply_id.is_none()
                                || reply_id == Some(ack_id)
                                || frame.data[2] != 0);
                    }
                    if reply_id != Some(frame.id) {
                        return false;
                    }
                    answer_is_ack = false;
                    page.is_none() || (frame.data[0] == page.unwrap_or(0) && frame.data[1] == seq)
                },
                Duration::from_millis(500),
            )?
            .ok_or_else(|| format!("{} timeout", yfoc_protocol::cmd_name(cmd)))?;
        if answer_is_ack {
            let ack = unpack_ack(&data);
            if ack.result != 0 {
                return Err(format!("{}: {}", ack.cmd_name, ack.result_name));
            }
        }
        Ok(data)
    }

    /// 查询能力。旧固件或超时返回 `None`，不报错。
    pub fn discover(&mut self) -> Result<Option<DeviceInfo>, String> {
        self.supports_current = false;
        self.config = None;
        self.position_warn = false;
        self.temp_sensor = false;
        let seq = self.next_seq();
        self.send(self.ids.mgmt, pack_mgmt(CMD_GET_DEVICE_INFO, seq, 0))?;
        let ack_id = self.ids.ack;
        let data = self.receive(
            |frame| {
                frame.id == ack_id && frame.data[0] == CMD_GET_DEVICE_INFO && frame.data[1] == seq
            },
            Duration::from_millis(500),
        )?;
        let Some(data) = data else {
            return Ok(None);
        };
        if data[2] != 0 {
            return Ok(None);
        }
        let info = unpack_device_info(&data);
        self.supports_current = info.current_loop;
        self.position_warn = info.position_warn;
        self.temp_sensor = info.temp_sensor;
        Ok(Some(info))
    }

    /// 读状态。内环位只在能力确认后解释。
    pub fn status(&mut self) -> Result<Status, String> {
        let data = self.mgmt(CMD_GET_STATUS, 0, Some(self.ids.status), None)?;
        Ok(self.parse_status(&data))
    }

    /// 请求输出轴位置、速度和指令力矩。普通反馈帧没有请求序号。
    pub fn feedback(&mut self) -> Result<yfoc_protocol::Feedback, String> {
        let data = self.mgmt(yfoc_protocol::CMD_GET_FEEDBACK, 0, Some(self.ids.fb), None)?;
        Ok(unpack_feedback(&data))
    }

    /// 等待普通实时命令触发的反馈，不额外发起查询。超时返回 None。
    pub fn recv_feedback(
        &mut self,
        timeout: Duration,
    ) -> Result<Option<yfoc_protocol::Feedback>, String> {
        let id = self.ids.fb;
        Ok(self
            .receive(|f| f.id == id, timeout)?
            .map(|data| unpack_feedback(&data)))
    }

    /// 切换单轴外环并通过状态回读确认。发送连续目标前执行一次。
    pub fn set_mode(&mut self, mode: ControlMode) -> Result<(), String> {
        self.mgmt(CMD_SET_MODE, mode as u8, None, None)?;
        let status = self.status()?;
        if status.mode != mode as u8 {
            return Err(format!("切换 {mode:?} 后节点报告 {}", status.mode_name));
        }
        Ok(())
    }

    /// 向位置模式发送一个绝对目标和速度上限。返回表示帧已提交给适配器。
    pub fn set_position(&mut self, rad: f64, max_rad_s: f64) -> Result<(), String> {
        use yfoc_protocol::{pack_position, POS_LSB, VEL_LSB};
        if !(f64::from(i32::MIN) * POS_LSB..=f64::from(i32::MAX) * POS_LSB).contains(&rad) {
            return Err("位置超出可编码范围".into());
        }
        if !(0.0..=f64::from(i16::MAX) * VEL_LSB).contains(&max_rad_s) {
            return Err("速度上限超出固件范围".into());
        }
        self.send(self.ids.pos, pack_position(rad, max_rad_s))
    }

    /// 向速度模式发送输出轴速度，单位 rad/s。
    pub fn set_velocity(&mut self, rad_s: f64) -> Result<(), String> {
        use yfoc_protocol::{pack_velocity, VEL_LSB};
        if !(f64::from(i16::MIN) * VEL_LSB..=f64::from(i16::MAX) * VEL_LSB).contains(&rad_s) {
            return Err("速度超出固件范围".into());
        }
        self.send(self.ids.vel, pack_velocity(rad_s))
    }

    /// 向本节点发送 DISABLE 并等待应答。关桥后该节点不再保持力矩。
    pub fn disable(&mut self) -> Result<(), String> {
        // Firmware caches its last management ACK even across ordinary motion frames.
        // On reconnect our first sequence can equal an old DISABLE. Two distinct
        // sequences guarantee that a fresh disable reaches the drive before its ACK.
        let seq = self.next_seq();
        self.send(self.ids.mgmt, pack_mgmt(yfoc_protocol::CMD_DISABLE, seq, 0))?;
        self.mgmt(yfoc_protocol::CMD_DISABLE, 0, None, None)?;
        Ok(())
    }

    /// 读取三页电流配置并记录本节点的 Iq 能力。
    pub fn current_config(&mut self) -> Result<CurrentConfig, String> {
        if !self.supports_current {
            return Err("Firmware does not advertise the current-loop protocol".into());
        }
        self.config = None;
        let mut config = CurrentConfig::default();
        for page in [0u8, 1, 2] {
            let data = self.mgmt(
                CMD_GET_CURRENT_CONFIG,
                page,
                Some(self.ids.current_config),
                Some(page),
            )?;
            config.apply_page(&unpack_current_config(&data).map_err(|err| err)?);
        }
        current_scale(Some(&config))?;
        self.config = Some(config.clone());
        Ok(config)
    }

    /// 切到 Motion 并写增益，两步都要成功 ACK。
    pub fn motion_setup(&mut self, kp: f64, kd: f64) -> Result<(), String> {
        self.mgmt(CMD_SET_MODE, MODE_MOTION, None, None)?;
        let seq = self.next_seq();
        self.send(self.ids.gains, pack_gains(MODE_MOTION, seq, kp, 0.0, kd))?;
        let ack_id = self.ids.ack;
        let data = self
            .receive(
                |frame| {
                    frame.id == ack_id && frame.data[0] == CMD_SET_GAINS && frame.data[1] == seq
                },
                Duration::from_millis(500),
            )?
            .ok_or_else(|| "SET_GAINS: timeout".to_string())?;
        let ack = unpack_ack(&data);
        if ack.result != 0 {
            return Err(format!("SET_GAINS: {}", ack.result_name));
        }
        Ok(())
    }

    /// 切换内环。
    pub fn set_inner(&mut self, current: bool) -> Result<(), String> {
        if !self.supports_current {
            return Err("Cannot switch inner loop: unsupported firmware".into());
        }
        let mode = if current {
            INNER_LOOP_CURRENT
        } else {
            INNER_LOOP_VOLTAGE
        };
        self.mgmt(CMD_SET_INNER_LOOP, mode, None, None)?;
        Ok(())
    }

    /// 读电流实测。
    pub fn current_state(&mut self) -> Result<yfoc_protocol::CurrentState, String> {
        if !self.supports_current {
            return Err("Measured current is unavailable on this firmware".into());
        }
        let data = self.mgmt(CMD_GET_CURRENT_STATE, 0, Some(self.ids.current), None)?;
        Ok(unpack_current_state(&data))
    }

    /// 收一帧，超时返回 `None`。
    pub fn recv(&mut self, timeout: Duration) -> Result<Option<Frame>, String> {
        self.bus.recv_timeout(timeout).map_err(|err| err.message)
    }

    /// 周期状态查询，序号独立递增。
    pub fn poke_status(&mut self) -> Result<(), String> {
        let seq = self.next_seq();
        self.send(self.ids.mgmt, pack_mgmt(CMD_GET_STATUS, seq, 0))
    }

    /// 请求一帧电流实测，回复由 CLI 的收帧循环处理。
    pub fn poke_current(&mut self) -> Result<(), String> {
        let seq = self.next_seq();
        self.send(self.ids.mgmt, pack_mgmt(CMD_GET_CURRENT_STATE, seq, 0))
    }

    /// 向已确认支持独立 Iq 的节点发送安培目标。须处于 Motion 外环和电流内环。
    pub fn set_direct_iq(&mut self, amps: f64) -> Result<(), String> {
        let data = pack_direct_iq(amps, self.config.as_ref())?;
        self.send(self.ids.iq_cmd, data)
    }

    /// 关闭前发出的零电流。
    pub fn send_zero_current(&mut self) -> Result<(), String> {
        self.set_direct_iq(0.0)
    }

    /// 解析反馈，供 CLI 打印。
    pub fn feedback_line(data: [u8; 8]) -> String {
        let fb = unpack_feedback(&data);
        format!(
            "fb pos={:+.4}rad vel={:+.3}rad/s torque={:+.3}Nm",
            fb.pos, fb.vel, fb.torque
        )
    }
}
