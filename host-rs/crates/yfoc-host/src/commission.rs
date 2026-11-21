//! Host-side fitting and supervised commissioning over Classic CAN.

mod fit;
#[cfg(test)]
mod tests;

use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU16, Ordering};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use clap::ValueEnum;
use serde_json::{json, Value};
use yfoc_can::{CanBus, Frame};

const INFO: u8 = 0x30;
const START: u8 = 0x31;
const STATUS: u8 = 0x32;
const SAMPLE: u8 = 0x33;
const STOP: u8 = 0x34;
const READ: u8 = 0x35;
const WRITE: u8 = 0x36;
const PROFILE: u8 = 0x37;

const ENC: u8 = 1;
const DIR: u8 = 2;
const RES: u8 = 3;
const LD: u8 = 4;
const LQ: u8 = 5;
const CURRENT: u8 = 6;
const VELOCITY: u8 = 7;
const POSITION: u8 = 8;
const COG: u8 = 9;
const COG_CHECK: u8 = 10;

const HAS_RL: u32 = 1;
const HAS_CURRENT: u32 = 2;
const HAS_PID: u32 = 4;
const HAS_ENC: u32 = 8;
const HAS_AUX: u32 = 16;
const HAS_COG: u32 = 32;
const TABLE: usize = 16;
const POINTS: usize = 256;
const WORDS: usize = TABLE + POINTS / 2;
type Sample = [u16; 3];
type Words = [u32; WORDS];

#[derive(Clone, Copy, Debug, PartialEq, Eq, ValueEnum)]
pub enum Step {
    Inspect,
    Encoder,
    Rl,
    Pid,
    Cog,
    Export,
}

impl Step {
    pub fn label(self) -> &'static str {
        match self {
            Self::Inspect => "读取节点",
            Self::Encoder => "编码器校准",
            Self::Rl => "电机 R/L 辨识",
            Self::Pid => "自动 PID 整定",
            Self::Cog => "齿槽转矩补偿",
            Self::Export => "导出参数",
        }
    }

    pub fn moves(self) -> bool {
        matches!(self, Self::Encoder | Self::Rl | Self::Pid | Self::Cog)
    }
}

#[derive(Clone, Debug)]
struct Info {
    board: u8,
    calibrated: bool,
    dual: bool,
    current_loop: bool,
    pole_hint: u8,
    teeth: [u8; 2],
    cpr: f64,
    aux_dir: f64,
    aux_tol: f64,
    gear: f64,
    kt: f64,
    vbus: f64,
    stored_flags: u32,
    abs_unknown: bool,
}

#[derive(Clone, Debug)]
pub struct Candidate {
    pub node: u8,
    pub step: Step,
    board: u8,
    before: Words,
    after: Words,
}

#[derive(Clone, Debug)]
pub struct ResultView {
    pub summary: String,
    pub candidate: Option<Candidate>,
    pub flags: u32,
}

#[derive(Clone, Debug)]
pub enum Action {
    Run { node: u8, step: Step },
    Save(Candidate),
    Export { node: u8, path: std::path::PathBuf },
    DisableCog { node: u8 },
}

impl Action {
    pub fn node(&self) -> u8 {
        match self {
            Self::Run { node, .. } | Self::Export { node, .. } | Self::DisableCog { node } => *node,
            Self::Save(candidate) => candidate.node,
        }
    }

    pub fn moves(&self) -> bool {
        matches!(self, Self::Run { step, .. } if step.moves())
    }
}

struct Link<'a> {
    bus: &'a mut dyn CanBus,
    node: u8,
    cancel: &'a AtomicBool,
    seq: u8,
    token: [u8; 2],
}

fn word(p: &[u8]) -> u16 {
    u16::from_le_bytes([p[0], p[1]])
}

fn error_name(n: u8) -> &'static str {
    match n {
        1 => "参数或 CRC 无效",
        2 => "设备忙",
        3 => "超过试验电流、速度、行程或电压限制",
        4 => "缺少有效电角度标定",
        5 => "驱动故障或传感器未就绪",
        6 => "心跳超时或已停止",
        7 => "采样数据无效",
        8 => "参数写入或回读校验失败",
        9 => "超过试验行程",
        10 => "超过试验速度",
        11 => "超过试验相电流",
        12 => "试验母线电压无效",
        _ => "未知错误",
    }
}

impl Link<'_> {
    fn request(&mut self, op: u8, payload: [u8; 6]) -> Result<[u8; 6], String> {
        if self.cancel.load(Ordering::Relaxed) {
            return Err("试验已停止".into());
        }
        self.seq = self.seq.wrapping_add(1);
        let mut data = [0; 8];
        data[..2].copy_from_slice(&[op, self.seq]);
        data[2..].copy_from_slice(&payload);
        let frame = Frame {
            id: 0x200 + u32::from(self.node),
            data,
        };
        let retries = if op == START || (op == PROFILE && payload[0] == 1) {
            1
        } else {
            3
        };
        let timeout = if op == PROFILE && payload[0] == 1 {
            1200
        } else {
            100
        };
        for _ in 0..retries {
            self.bus.send(&frame).map_err(|e| e.to_string())?;
            let end = Instant::now() + Duration::from_millis(timeout);
            while Instant::now() < end {
                if self.cancel.load(Ordering::Relaxed) && !(op == PROFILE && payload[0] == 1) {
                    return Err("试验已停止".into());
                }
                let Some(reply) = self
                    .bus
                    .recv_timeout(Duration::from_millis(4))
                    .map_err(|e| e.to_string())?
                else {
                    continue;
                };
                if reply.id != 0x480 + u32::from(self.node) || reply.data[1] != self.seq {
                    continue;
                }
                if reply.data[0] == op | 0x80 {
                    return Err(format!("{}（0x{op:02X}）", error_name(reply.data[2])));
                }
                if reply.data[0] == op {
                    return Ok(reply.data[2..].try_into().expect("six bytes"));
                }
            }
        }
        if op == INFO {
            Err(format!(
                "节点 {} 没有响应校准协议。请核对节点号、CAN 和固件版本",
                self.node
            ))
        } else {
            Err(format!("校准通信超时（0x{op:02X}）"))
        }
    }

    fn stop(&mut self) {
        let stop = Frame {
            id: 0x200 + u32::from(self.node),
            data: [STOP, 0, 0, 0, 0, 0, 0, 0],
        };
        let disable = Frame {
            id: stop.id,
            data: yfoc_protocol::pack_mgmt(yfoc_protocol::CMD_DISABLE, 0xFE, 0),
        };
        let _ = self.bus.send(&stop);
        let _ = self.bus.send(&disable);
    }

    fn info(&mut self) -> Result<Info, String> {
        let a = self.request(INFO, [0; 6])?;
        if a[0] != 1 {
            return Err(format!("上位机不支持试验协议 v{}", a[0]));
        }
        let b = self.request(INFO, [1, 0, 0, 0, 0, 0])?;
        let c = self.request(INFO, [2, 0, 0, 0, 0, 0])?;
        Ok(Info {
            board: a[1],
            calibrated: a[2] & 1 != 0,
            dual: a[2] & 2 != 0,
            current_loop: a[2] & 4 != 0,
            pole_hint: a[3],
            teeth: [a[4], a[5]],
            cpr: match b[0] {
                14 => 16384.0,
                16 => 65536.0,
                _ => return Err("编码器分辨率未知".into()),
            },
            aux_dir: f64::from(b[1] as i8),
            aux_tol: f64::from(word(&b[4..])) * 0.001,
            gear: f64::from(word(&b[2..])) * 0.001,
            vbus: f64::from(word(&c)) * 0.001,
            kt: f64::from(word(&c[2..])) * 0.001,
            stored_flags: u32::from(c[4]),
            abs_unknown: c[5] != 0,
        })
    }

    fn profile(&mut self) -> Result<Words, String> {
        self.request(PROFILE, [0; 6])?;
        let mut w = [0; WORDS];
        for (i, value) in w.iter_mut().enumerate() {
            // The device retains table words when a map is disabled, for round trips.
            let data = self.request(READ, [i as u8, 0, 0, 0, 0, 0])?;
            *value = u32::from_le_bytes(data[..4].try_into().expect("four bytes"));
        }
        Ok(w)
    }

    fn stage(&mut self, previous: &Words, next: &Words) -> Result<(), String> {
        for (i, (&old, &new)) in previous.iter().zip(next).enumerate() {
            if old == new {
                continue;
            }
            let mut data = [i as u8, 0, 0, 0, 0, 0];
            data[2..].copy_from_slice(&new.to_le_bytes());
            self.request(WRITE, data)?;
        }
        Ok(())
    }

    fn commit(&mut self, next: &Words) -> Result<(), String> {
        let mut crc = crc32fast::Hasher::new();
        for value in next {
            crc.update(&value.to_le_bytes());
        }
        let mut data = [1, 0, 0, 0, 0, 0];
        data[2..].copy_from_slice(&crc.finalize().to_le_bytes());
        self.request(PROFILE, data)?;
        let saved = self.profile()?;
        if saved[0] != next[0]
            || (next[0] & HAS_ENC != 0 && saved[6..8] != next[6..8])
            || (next[0] & HAS_RL != 0 && saved[1..4] != next[1..4])
            || (next[0] & HAS_CURRENT != 0 && saved[4..6] != next[4..6])
            || (next[0] & HAS_PID != 0 && saved[8..13] != next[8..13])
            || (next[0] & HAS_COG != 0 && saved[TABLE..] != next[TABLE..])
        {
            return Err("设备参数回读与提交值不同".into());
        }
        Ok(())
    }

    fn trial(&mut self, kind: u8, variant: i8, arg: i16) -> Result<Vec<Sample>, String> {
        let token = self.token;
        let [lo, hi] = arg.to_le_bytes();
        self.request(START, [kind, variant as u8, lo, hi, token[0], token[1]])?;
        let started = Instant::now();
        let count = loop {
            let state = self.request(STATUS, [token[0], token[1], 0, 0, 0, 0])?;
            if state[1] != kind {
                return Err("试验类型与请求不一致".into());
            }
            if state[0] == 3 && state[5] != 0 {
                break word(&state[2..]) as usize;
            }
            if state[0] == 4 {
                return Err(format!(
                    "试验 {kind} 已停机。{}，已采集 {} 点",
                    error_name(state[4]),
                    word(&state[2..])
                ));
            }
            if started.elapsed() > Duration::from_secs(20) {
                return Err("试验超过最长允许时间".into());
            }
            thread::sleep(Duration::from_millis(38));
        };
        if !(8..=2048).contains(&count) {
            return Err(format!("试验 {kind} 样本数 {count} 无效"));
        }
        let mut samples = Vec::with_capacity(count);
        for i in 0..count {
            let [lo, hi] = (i as u16).to_le_bytes();
            let p = self.request(SAMPLE, [lo, hi, token[0], token[1], 0, 0])?;
            samples.push([word(&p), word(&p[2..]), word(&p[4..])]);
        }
        Ok(samples)
    }
}

pub fn execute(
    bus: &mut dyn CanBus,
    action: Action,
    cancel: &AtomicBool,
    mut progress: impl FnMut(String),
) -> Result<ResultView, String> {
    if !(1..=63).contains(&action.node()) {
        return Err("节点号须为 1～63".into());
    }
    static NEXT_TOKEN: AtomicU16 = AtomicU16::new(1);
    let mut token = NEXT_TOKEN.fetch_add(1, Ordering::Relaxed);
    if token == 0 {
        token = NEXT_TOKEN.fetch_add(1, Ordering::Relaxed);
    }
    let mut link = Link {
        bus,
        node: action.node(),
        cancel,
        seq: 0,
        token: token.to_le_bytes(),
    };
    let stop = action.moves();
    let result = execute_action(&mut link, action, &mut progress);
    if stop {
        link.stop();
    }
    result
}

fn execute_action(
    link: &mut Link<'_>,
    action: Action,
    progress: &mut dyn FnMut(String),
) -> Result<ResultView, String> {
    let info = link.info()?;
    if action.moves() {
        link.stop();
    }
    let live = link.profile()?;
    let mut after = live;
    let mut candidate = None;
    let summary = match action {
        Action::Run {
            step: Step::Inspect,
            ..
        } => {
            format!(
                "节点 {}，{}，母线 {:.2} V，减速比 {:.1}，{}。{}",
                link.node,
                board_name(info.board),
                info.vbus,
                info.gear,
                if info.calibrated {
                    "已有电角度标定"
                } else {
                    "等待电角度标定"
                },
                if info.abs_unknown {
                    "多圈位置未建立。"
                } else {
                    ""
                }
            )
        }
        Action::Run { step, .. } => {
            let report = match step {
                Step::Encoder => encoder(link, &info, &live, &mut after, progress)?,
                Step::Rl => identify(link, &info, &mut after, progress)?,
                Step::Pid => tune(link, &info, &live, &mut after, progress)?,
                Step::Cog => cogging(link, &info, &live, &mut after, progress)?,
                _ => return Err("无效的操作".into()),
            };
            candidate = Some(Candidate {
                node: link.node,
                step,
                board: info.board,
                before: live,
                after,
            });
            report
        }
        Action::Save(c) => {
            if c.board != info.board || c.before != live {
                return Err("设备参数已变化，请重新运行该步骤".into());
            }
            progress("正在写入参数并回读校验".into());
            link.stage(&live, &c.after)?;
            link.commit(&c.after)?;
            after = c.after;
            format!("{}已保存，设备回读一致", c.step.label())
        }
        Action::Export { path, .. } => {
            let zero = link.request(INFO, [3, 0, 0, 0, 0, 0])?;
            let zero = f32::from_le_bytes(zero[..4].try_into().expect("four bytes"));
            export(&path, link.node, &info, &live, zero)?;
            format!("参数已导出到 {}", path.display())
        }
        Action::DisableCog { .. } => {
            if live[0] & HAS_COG == 0 {
                return Err("设备当前没有启用齿槽补偿".into());
            }
            after[0] &= !HAS_COG;
            link.stage(&live, &after)?;
            link.commit(&after)?;
            "齿槽补偿已关闭并保存".into()
        }
    };
    let flags = if candidate.is_some() {
        live[0]
    } else {
        after[0]
    };
    Ok(ResultView {
        summary,
        candidate,
        flags,
    })
}

fn encoder(
    link: &mut Link<'_>,
    info: &Info,
    live: &Words,
    next: &mut Words,
    progress: &mut dyn FnMut(String),
) -> Result<String, String> {
    let voltage =
        (info.vbus * 180.0).clamp(2000.0, if info.board == 1 { 4400.0 } else { 2900.0 }) as i16;
    progress("编码器正向扫转".into());
    let a = link.trial(ENC, 1, voltage)?;
    progress("编码器反向扫转".into());
    let b = link.trial(ENC, -1, voltage)?;
    let align = fit::encoder(info, [&a, &b])?;
    next[0] = (live[0] | HAS_ENC) & !(HAS_AUX | HAS_COG);
    if align.aux.is_some() {
        next[0] |= HAS_AUX;
    }
    next[6] = u32::from_le_bytes([
        align.poles,
        align.direction as u8,
        1,
        align.aux.unwrap_or(0) as u8,
    ]);
    next[7] = (align.offset as f32).to_bits();
    link.stage(live, next)?;
    progress("短时 q 轴激励验证转矩方向".into());
    let positive = link.trial(DIR, 0, if info.board == 1 { 2200 } else { 1500 })?;
    let d = positive.last().map_or(0, |s| s[0] as i16);
    if d.abs() < 45 {
        return Err(format!("闭环方向验证位移仅 {d} mrad，无法可靠判定"));
    }
    next[6] = (next[6] & !(255 << 16)) | (u32::from(d.signum() as u8) << 16);
    let aux = match align.aux {
        Some(raw) => format!("，游标零偏 {raw}/256"),
        None => String::new(),
    };
    Ok(format!(
        "{} 极对，编码器方向 {:+}，转矩方向 {:+}，电角偏置 {:.4} rad，残差 P95 {:.3} rad{aux}",
        align.poles,
        align.direction,
        d.signum(),
        align.offset,
        align.p95
    ))
}

fn identify(
    link: &mut Link<'_>,
    info: &Info,
    next: &mut Words,
    progress: &mut dyn FnMut(String),
) -> Result<String, String> {
    if !info.calibrated {
        return Err("请先完成编码器校准".into());
    }
    progress("采集 d 轴稳态电压与电流".into());
    let amplitude = if info.board == 1 { 4750 } else { 2500 };
    let r1 = fit::resistance(&link.trial(RES, 0, amplitude)?)?;
    let r2 = fit::resistance(&link.trial(RES, 0, amplitude * 4 / 5)?)?;
    let rs = (r1 + r2) * 0.5;
    if (r1 - r2).abs() > 0.18 * rs {
        return Err(format!("两次相电阻辨识结果不一致（{r1:.3} / {r2:.3} Ω）"));
    }
    let mv = (rs * 275.0).clamp(450.0, 2200.0) as i16;
    progress(format!("相电阻 {rs:.3} Ω，采集 d 轴电感"));
    let ld = fit::inductance(&link.trial(LD, 0, mv)?, rs)?;
    progress("采集 q 轴电感".into());
    let lq = fit::inductance(&link.trial(LQ, 0, mv)?, rs)?;
    set_motor_rl(next, rs, ld, lq);
    Ok(format!(
        "相电阻 Rs {:.4} Ω，Ld {:.3} mH，Lq {:.3} mH。保存后需重新整定 PID 并生成齿槽补偿表。",
        rs,
        ld * 1000.0,
        lq * 1000.0
    ))
}

fn set_motor_rl(profile: &mut Words, rs: f64, ld: f64, lq: f64) {
    profile[0] = (profile[0] | HAS_RL) & !(HAS_CURRENT | HAS_PID | HAS_COG);
    profile[1] = (rs as f32).to_bits();
    profile[2] = (ld as f32).to_bits();
    profile[3] = (lq as f32).to_bits();
}

fn current_bandwidths(board: u8) -> &'static [f32] {
    match board {
        1 => &[180.0, 260.0, 350.0, 425.0],
        _ => &[180.0, 260.0, 350.0],
    }
}

struct CurrentCandidate {
    bandwidth: f32,
    kp: f32,
    ki: f32,
    scores: Vec<f64>,
}

impl CurrentCandidate {
    fn median_score(&mut self, rounds: usize) -> Option<f64> {
        if self.scores.len() != rounds {
            return None;
        }
        self.scores.sort_by(f64::total_cmp);
        Some(self.scores[rounds / 2])
    }
}

fn tune(
    link: &mut Link<'_>,
    info: &Info,
    live: &Words,
    next: &mut Words,
    progress: &mut dyn FnMut(String),
) -> Result<String, String> {
    if !info.calibrated || live[0] & HAS_RL == 0 {
        return Err("请先保存编码器校准和 R/L 辨识结果".into());
    }
    if !info.current_loop {
        return Err("请在电机页切换到电流内环".into());
    }
    let mut stage = *live;
    next[0] = (live[0] | HAS_CURRENT | HAS_PID) & !HAS_COG;
    let rs = f32::from_bits(live[1]);
    let l = (f32::from_bits(live[2]) + f32::from_bits(live[3])) * 0.5;
    let rounds = if info.board == 1 { 3 } else { 1 };
    let mut candidates = Vec::new();
    for &bw in current_bandwidths(info.board) {
        let wc = std::f32::consts::TAU * bw;
        let (kp, ki) = (l * wc, rs * wc);
        // Firmware v1 checks these limits before arming a trial.
        if !(0.01..=22.0).contains(&kp) || !(1.0..=60000.0).contains(&ki) {
            progress(format!("跳过 {bw:.0} Hz，PI 增益超出固件试验范围"));
            continue;
        }
        candidates.push(CurrentCandidate {
            bandwidth: bw,
            kp,
            ki,
            scores: Vec::new(),
        });
    }
    for round in 0..rounds {
        for c in &mut candidates {
            if c.scores.len() != round {
                continue;
            }
            progress(format!(
                "电流 PI {:.0} Hz，第 {}/{rounds} 轮",
                c.bandwidth,
                round + 1
            ));
            next[4] = c.kp.to_bits();
            next[5] = c.ki.to_bits();
            link.stage(&stage, next)?;
            stage = *next;
            let first = if round % 2 == 0 { 280 } else { -280 };
            let a = fit::current_score(&link.trial(CURRENT, 0, first)?);
            let b = fit::current_score(&link.trial(CURRENT, 0, -first)?);
            if let (Some(a), Some(b)) = (a, b) {
                c.scores.push(a + b);
            }
        }
        candidates.reverse();
    }
    let (_, best) = candidates
        .into_iter()
        .filter_map(|mut c| c.median_score(rounds).map(|score| (score, c)))
        .min_by(|(a, ca), (b, cb)| a.total_cmp(b).then(ca.bandwidth.total_cmp(&cb.bandwidth)))
        .ok_or("电流 PI 候选都未通过正反阶跃验证")?;
    next[4] = best.kp.to_bits();
    next[5] = best.ki.to_bits();
    let mut best_vel = (f64::INFINITY, 0.0f32, 0.0f32);
    let baseline_kp = f32::from_bits(live[8]);
    let baseline_ki = f32::from_bits(live[9]);
    let baseline = if baseline_kp > 0.0005 && baseline_ki > 0.001 {
        (baseline_kp.min(0.035), baseline_ki.min(0.30))
    } else {
        (0.008, 0.08)
    };
    for scale in [0.8f32, 1.0, 1.3] {
        let kp = baseline.0 * scale;
        let ki = baseline.1 * scale;
        progress(format!("速度 PI 候选 Kp {kp:.4}，Ki {ki:.4}"));
        next[8] = kp.to_bits();
        next[9] = ki.to_bits();
        link.stage(&stage, next)?;
        stage = *next;
        let a = link.trial(VELOCITY, 0, 650)?;
        let b = link.trial(VELOCITY, 0, -650)?;
        if let (Some(a), Some(b)) = (
            fit::motion_score(&a, 650, false),
            fit::motion_score(&b, -650, false),
        ) {
            if a + b < best_vel.0 {
                best_vel = (a + b, kp, ki);
            }
        }
    }
    if !best_vel.0.is_finite() {
        return Err("速度 PI 候选都未达到双向跟踪要求".into());
    }
    next[8] = best_vel.1.to_bits();
    next[9] = best_vel.2.to_bits();
    let mut best_pos = (f64::INFINITY, 0.0f32, 0.0f32, 0.0f32);
    for i in 0..5 {
        let (kp, ki, kd): (f32, f32, f32) = match i {
            0..=2 => ([10.0, 20.0, 30.0][i], 0.0, 0.0),
            3 if best_pos.0.is_finite() => (best_pos.1, 0.0, 0.12),
            4 if best_pos.0.is_finite() => (best_pos.1, 0.4, 0.0),
            _ => break,
        };
        progress(format!(
            "位置 PID 候选 {kp:.1} / {ki:.2} / {kd:.2}，往返 0.08 rad"
        ));
        next[10] = kp.to_bits();
        next[11] = ki.to_bits();
        next[12] = kd.to_bits();
        link.stage(&stage, next)?;
        stage = *next;
        let a = link.trial(POSITION, 0, 80)?;
        let b = link.trial(POSITION, 0, -80)?;
        if let (Some(a), Some(b)) = (
            fit::motion_score(&a, 80, true),
            fit::motion_score(&b, -80, true),
        ) {
            let required_improvement = if i < 3 { 1.0 } else { 1.08 };
            if (a + b) * required_improvement < best_pos.0 {
                best_pos = (a + b, kp, ki, kd);
            }
        }
    }
    if !best_pos.0.is_finite() {
        return Err("位置 PID 候选都未通过往返验证".into());
    }
    next[10] = best_pos.1.to_bits();
    next[11] = best_pos.2.to_bits();
    next[12] = best_pos.3.to_bits();
    link.stage(&stage, next)?;
    progress("验证位置参数，限速往返 0.20 rad".into());
    for arg in [200, -200] {
        if fit::motion_score(&link.trial(POSITION, 0, arg)?, arg, true).is_none() {
            return Err("位置 PID 在独立往返试验中未通过".into());
        }
    }
    Ok(format!("电流 PI {:.3} / {:.0}（设计带宽 {:.0} Hz），速度 PI {:.5} / {:.5}，位置 PID {:.1} / {:.2} / {:.2}。已通过正反阶跃及位置往返验证",
        best.kp, best.ki, best.bandwidth, best_vel.1, best_vel.2, best_pos.1, best_pos.2, best_pos.3))
}

fn cogging(
    link: &mut Link<'_>,
    info: &Info,
    live: &Words,
    next: &mut Words,
    progress: &mut dyn FnMut(String),
) -> Result<String, String> {
    if !info.calibrated || !info.current_loop || live[0] & HAS_PID == 0 {
        return Err("请先保存电角度和 PID 整定结果，并使用电流内环".into());
    }
    let velocity = (3500.0 / info.gear).clamp(70.0, 3500.0) as i16;
    progress("关闭补偿，正转采集角度与 Iq".into());
    let fwd = link.trial(COG, 0, velocity)?;
    progress("关闭补偿，反转采集角度与 Iq".into());
    let rev = link.trial(COG, 0, -velocity)?;
    let map = fit::cogging(info, [&fwd, &rev], velocity)?;
    next[0] |= HAS_COG;
    for (i, pair) in map.chunks_exact(2).enumerate() {
        next[TABLE + i] = u32::from(pair[0] as u16) | (u32::from(pair[1] as u16) << 16);
    }
    link.stage(live, next)?;
    progress("临时启用补偿，复测双向速度纹波".into());
    let on_fwd = link.trial(COG_CHECK, 0, velocity)?;
    let on_rev = link.trial(COG_CHECK, 0, -velocity)?;
    let off = fit::ripple(&fwd, &rev, velocity)?;
    let on = fit::ripple(&on_fwd, &on_rev, velocity)?;
    if on.0 >= off.0 * 0.98 || on.1 > off.1 + 0.18 {
        return Err(format!(
            "补偿未通过复测，速度纹波 {:.4} → {:.4} rad/s，峰值电流 {:.3} → {:.3} A",
            off.0, on.0, off.1, on.1
        ));
    }
    Ok(format!(
        "{} 点补偿表，速度纹波 RMS {:.4} → {:.4} rad/s，峰值电流 {:.3} → {:.3} A",
        POINTS, off.0, on.0, off.1, on.1
    ))
}

fn board_name(board: u8) -> &'static str {
    match board {
        1 => "YH_JM",
        _ => "未知板卡",
    }
}

fn export(
    path: &Path,
    node: u8,
    info: &Info,
    w: &Words,
    zero_motor_rad: f32,
) -> Result<(), String> {
    let f = |i: usize| f32::from_bits(w[i]);
    let enabled = |flag| w[0] & flag != 0;
    let [poles, dir, torque, aux] = w[6].to_le_bytes();
    let table: Vec<i16> = w[TABLE..]
        .iter()
        .flat_map(|item| [*item as i16, (item >> 16) as i16])
        .collect();
    let profile = json!({
        "schema": "yfoc.motor-parameters", "version": 1,
        "created_unix_s": SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs(),
        "board": board_name(info.board), "node": node, "gear_ratio": info.gear,
        "motor_kt_nm_per_a": info.kt, "vbus_at_export_v": info.vbus,
        "stored_flags": info.stored_flags, "operating_inner_loop": if info.current_loop { "current" } else { "voltage" },
        "encoder": if enabled(HAS_ENC) { json!({
            "pole_pairs": poles, "encoder_direction": dir as i8, "torque_direction": torque as i8,
            "electrical_offset_rad": f(7), "vernier_offset_turns": enabled(HAS_AUX).then_some(f64::from(aux as i8) / 256.0),
            "absolute_multiturn_ready": info.dual && !info.abs_unknown, "mechanical_zero_motor_rad": zero_motor_rad,
        }) } else { Value::Null },
        "motor_rl": enabled(HAS_RL).then(|| json!({"phase_resistance_ohm": f(1), "ld_h": f(2), "lq_h": f(3)})),
        "current_pi": {"kp_v_per_a": f(4), "ki_v_per_a_s": f(5), "autotuned": enabled(HAS_CURRENT)},
        "velocity_pi": {"kp": f(8), "ki": f(9), "output_unit": "pu", "feedback_axis": "output", "autotuned": enabled(HAS_PID)},
        "position_pid": {"kp": f(10), "ki": f(11), "kd": f(12), "feedback_axis": "output", "autotuned": enabled(HAS_PID)},
        "cogging": {"enabled": enabled(HAS_COG), "index": "raw_motor_encoder_single_turn",
                    "iq_ma": enabled(HAS_COG).then_some(table)},
    });
    let json = serde_json::to_vec_pretty(&profile).map_err(|e| e.to_string())?;
    std::fs::write(path, json).map_err(|e| format!("写入 {} 失败，{e}", path.display()))
}
