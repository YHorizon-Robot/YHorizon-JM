//! 固件 1.6 预装一至两段位置轨迹，共同启动并以 200 Hz 反馈。
//! START 后的同步帧仅携带周期号。每拍的各轴反馈通过 [`Event::Cycle`] 返回。
use std::collections::{BTreeMap, HashSet};
use std::sync::atomic::{AtomicBool, AtomicU16, Ordering};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use yfoc_can::{CanBus, Frame};

pub const PERIOD: Duration = Duration::from_millis(5);
pub const MAX_AXES: usize = 16;
const SYNC: u32 = 0x080;
const CAPS: u8 = 0x40;
const TARGET: u8 = 0x41;
const ARM: u8 = 0x42;
const SECOND: u8 = 0x43;
const LIMITS: u8 = 0x44;

#[derive(Clone, Debug)]
pub struct Axis {
    pub node: u8,
    pub target: f64,
    pub second: Option<f64>,
}

#[derive(Clone, Debug)]
pub struct Plan {
    pub axes: Vec<Axis>,
    pub seconds: f64,
}

impl Plan {
    pub fn validate(&self) -> Result<(), String> {
        if self.axes.is_empty()
            || self.axes.len() > MAX_AXES
            || !(0.2..=60.0).contains(&self.seconds)
        {
            return Err("需要 1～16 个轴，每段时长 0.2～60 秒".into());
        }
        let mut ids = HashSet::new();
        for axis in &self.axes {
            if !(1..=63).contains(&axis.node) || !ids.insert(axis.node) {
                return Err("节点号应为 1～63，组内节点不可重复".into());
            }
            if axis.second.is_some() != self.axes[0].second.is_some() {
                return Err("所有轴必须配置相同的段数".into());
            }
            if std::iter::once(axis.target)
                .chain(axis.second)
                .any(|x| !(-2000000.0..=2000000.0).contains(&x))
            {
                return Err("输出轴目标超出编码范围".into());
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone)]
pub struct Sample {
    pub host_us: u64,
    pub cycle: u16,
    pub node: u8,
    pub reference: f64,
    pub position: f64,
    pub velocity: f64,
}

pub enum Event {
    Status(String),
    Cycle(Vec<Sample>),
}

struct AxisState {
    origin: f64,
    latest: Option<u16>,
    count: usize,
}

fn u16le(data: &[u8]) -> u16 {
    u16::from_le_bytes([data[0], data[1]])
}
fn i32le(data: &[u8]) -> i32 {
    i32::from_le_bytes(data[..4].try_into().unwrap())
}
fn mm(x: f64) -> i32 {
    (x * 1000.0).round() as i32
}

fn reference(axis: &Axis, origin: f64, cycle: u16, duration: u16) -> f64 {
    let second = axis.second.is_some() && cycle >= duration;
    let (from, to, elapsed) = if second {
        (axis.target, axis.second.unwrap(), cycle - duration)
    } else {
        (origin, axis.target, cycle)
    };
    let u = (f64::from(elapsed) / f64::from(duration)).min(1.0);
    from + (to - from) * u.powi(3) * (10.0 + u * (-15.0 + 6.0 * u))
}

struct Link<'a> {
    bus: &'a mut dyn CanBus,
    cancel: &'a AtomicBool,
    token: u16,
    seq: u8,
}

impl Link<'_> {
    fn check(&self) -> Result<(), String> {
        if self.cancel.load(Ordering::Acquire) {
            Err("同步运动已取消".into())
        } else {
            Ok(())
        }
    }
    fn send(&mut self, id: u32, data: [u8; 8]) -> Result<(), String> {
        self.bus
            .send(&Frame { id, data })
            .map_err(|e| e.to_string())
    }
    fn clock(&mut self, op: u8, cycle: u16) -> Result<(), String> {
        let mut d = [0; 8];
        d[0] = op;
        d[2..4].copy_from_slice(&self.token.to_le_bytes());
        d[4..6].copy_from_slice(&cycle.to_le_bytes());
        self.send(SYNC, d)
    }
    fn request(&mut self, node: u8, op: u8, value: [u8; 4]) -> Result<[u8; 8], String> {
        self.seq = self.seq.wrapping_add(1);
        let mut d = [op, self.seq, 0, 0, 0, 0, 0, 0];
        d[2..4].copy_from_slice(&self.token.to_le_bytes());
        d[4..].copy_from_slice(&value);
        for _ in 0..2 {
            self.check()?;
            self.send(0x200 + u32::from(node), d)?;
            let until = Instant::now() + Duration::from_millis(80);
            while Instant::now() < until {
                self.check()?;
                if let Some(f) = self
                    .bus
                    .recv_timeout(Duration::from_millis(1))
                    .map_err(|e| e.to_string())?
                {
                    if f.id != 0x500 + u32::from(node) || f.data[..2] != d[..2] {
                        continue;
                    }
                    if f.data[2] != 0 || f.data[3] != 1 {
                        let reason = match f.data[2] {
                            1 => "请求无效",
                            2 => "节点忙",
                            3 => "标定、内环、增益、位置或静止条件未就绪",
                            4 => "行程或轨迹限值",
                            _ => "同步协议版本不匹配",
                        };
                        return Err(format!("节点 {node} 命令 {op:#04x}：{reason}"));
                    }
                    return Ok(f.data);
                }
            }
        }
        Err(format!(
            "节点 {node} 未应答 {op:#04x}，确认节点在线并支持同步协议 v1"
        ))
    }
}

/// 阻塞执行组轨迹。总线须为 1 Mbit/s，此调用期间独占收发。
/// 将 `cancel` 置为 true 可取消。回调在 CAN 调度线程内执行，耗时工作应转交其他线程。
/// 返回成功前，逐轴确认已退出本次运动。
pub fn execute(
    bus: &mut dyn CanBus,
    plan: &Plan,
    cancel: &AtomicBool,
    mut event: impl FnMut(Event),
) -> Result<String, String> {
    plan.validate()?;
    static NONCE: AtomicU16 = AtomicU16::new(1);
    let salt = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .subsec_nanos() as u16;
    let token = (salt ^ NONCE.fetch_add(1, Ordering::Relaxed)).max(1);
    let mut link = Link {
        bus,
        cancel,
        token,
        seq: token as u8,
    };
    let result = run(&mut link, plan, &mut event);
    // Token-specific stop also invalidates unstarted work after a partial preparation.
    let stop = link.clock(3, 0);
    if result.is_err() || stop.is_err() {
        let _ = link.clock(3, 0);
    }
    let summary = result?;
    stop?;
    std::thread::sleep(Duration::from_millis(2));
    for axis in &plan.axes {
        link.request(axis.node, CAPS, [0; 4])
            .map_err(|e| format!("结束后未确认节点空闲：{e}"))?;
    }
    Ok(summary)
}

fn run(link: &mut Link<'_>, plan: &Plan, event: &mut impl FnMut(Event)) -> Result<String, String> {
    let mut axes = Vec::new();
    let mut seconds = plan.seconds;
    for axis in &plan.axes {
        let info = link.request(axis.node, CAPS, [0; 4])?;
        let origin = f64::from(i32le(&info[4..])) * 0.001;
        let limits = link.request(axis.node, LIMITS, [0; 4])?;
        let vmax = f64::from(u16le(&limits[4..])) * 0.001;
        let acc = f64::from(u16le(&limits[6..])) * 0.001;
        let distance = (axis.target - origin)
            .abs()
            .max((axis.second.unwrap_or(axis.target) - axis.target).abs());
        if distance > 30.0
            || (axis.second.unwrap_or(axis.target) - origin).abs() > 30.0
            || vmax <= 0.0
            || acc <= 0.0
        {
            return Err(format!("节点 {} 的目标或限值超出同步轨迹范围", axis.node));
        }
        seconds = seconds
            .max(1.875 * distance / vmax)
            .max((5.775 * distance / acc).sqrt());
        axes.push(AxisState {
            origin,
            latest: None,
            count: 0,
        });
    }
    if seconds > 60.0 {
        return Err("满足轴限值所需时长超过 60 秒".into());
    }
    let duration = (seconds * 200.0).ceil() as u16;
    let total = duration * if plan.axes[0].second.is_some() { 2 } else { 1 };
    let staging_started = Instant::now();
    for (slot, (axis, state)) in plan.axes.iter().zip(&mut axes).enumerate() {
        link.request(axis.node, TARGET, mm(axis.target).to_le_bytes())?;
        if let Some(goal) = axis.second {
            link.request(axis.node, SECOND, mm(goal).to_le_bytes())?;
        }
        let [lo, hi] = duration.to_le_bytes();
        let ack = link.request(
            axis.node,
            ARM,
            [lo, hi, slot as u8, axis.second.is_some() as u8],
        )?;
        state.origin = f64::from(i32le(&ack[4..])) * 0.001;
        event(Event::Status(format!(
            "节点 {} 就绪（{}/{}），每段 {:.3} 秒",
            axis.node,
            slot + 1,
            plan.axes.len(),
            f64::from(duration) / 200.0
        )));
    }
    let mut index = [None; 64];
    for (i, a) in plan.axes.iter().enumerate() {
        index[a.node as usize] = Some(i);
    }
    let mut pending: BTreeMap<u16, Vec<Option<Sample>>> = BTreeMap::new();
    let start = Instant::now();
    let mut last = start;
    let mut cycle = 0u16;
    let mut clocks = 1usize;
    let mut complete = 0usize;
    let mut stable = 0;
    let mut last_complete = None;
    let mut max_gap = Duration::ZERO;
    link.check()?;
    if staging_started.elapsed() >= Duration::from_millis(1700) {
        return Err("准备超时，已取消本次同步启动".into());
    }
    link.clock(1, 0)?;
    event(Event::Status("运行中 · 同步和每轴反馈均为 200 Hz".into()));
    loop {
        link.check()?;
        let elapsed = start.elapsed();
        let due =
            u16::try_from(elapsed.as_micros() / PERIOD.as_micros()).map_err(|_| "主机调度超时")?;
        if due > cycle {
            let gap = last.elapsed();
            if due - cycle > 4 || gap >= Duration::from_millis(24) {
                return Err(format!("主机调度超时 {:.2} ms", gap.as_secs_f64() * 1000.0));
            }
            if let Some((i, _)) = axes
                .iter()
                .enumerate()
                .find(|(_, s)| due.saturating_sub(s.latest.unwrap_or(0)) > 4)
            {
                return Err(format!("节点 {} 反馈超时，整组停止", plan.axes[i].node));
            }
            if due > total + 400 {
                return Err("轨迹结束后 2 秒内未全部到位".into());
            }
            cycle = due;
            link.clock(2, cycle)?;
            clocks += 1;
            max_gap = max_gap.max(gap);
            last = Instant::now();
            pending.retain(|&k, _| cycle.saturating_sub(k) <= 4);
        }
        let Some(f) = link
            .bus
            .recv_timeout(Duration::ZERO)
            .map_err(|e| e.to_string())?
        else {
            std::thread::sleep(Duration::from_micros(100));
            continue;
        };
        if (0x381..=0x3bf).contains(&f.id)
            && index[(f.id - 0x380) as usize].is_some()
            && u16le(&f.data[1..]) != 0
        {
            return Err(format!("节点 {} 报告驱动故障", f.id - 0x380));
        }
        if !(0x541..=0x57f).contains(&f.id) || u16le(&f.data) != link.token {
            continue;
        }
        let node = (f.id - 0x540) as u8;
        let Some(i) = index[usize::from(node)] else {
            continue;
        };
        let seq = u16le(&f.data[2..]);
        let state = &mut axes[i];
        if seq > cycle || cycle - seq > 4 || state.latest.is_some_and(|s| seq <= s) {
            continue;
        }
        let row = pending
            .entry(seq)
            .or_insert_with(|| vec![None; plan.axes.len()]);
        state.latest = Some(seq);
        state.count += 1;
        row[i] = Some(Sample {
            node,
            cycle: seq,
            host_us: start.elapsed().as_micros() as u64,
            reference: reference(&plan.axes[i], state.origin, seq, duration),
            position: state.origin + f64::from(u16le(&f.data[4..]) as i16) * 0.001,
            velocity: f64::from(u16le(&f.data[6..]) as i16) * 0.001,
        });
        if row.iter().all(Option::is_some) {
            let data: Vec<_> = pending
                .remove(&seq)
                .unwrap()
                .into_iter()
                .flatten()
                .collect();
            complete += 1;
            let arrived = seq >= total
                && data
                    .iter()
                    .all(|x| (x.reference - x.position).abs() <= 0.015 && x.velocity.abs() <= 0.10);
            if !arrived || last_complete != seq.checked_sub(1) {
                stable = 0;
            }
            if arrived {
                stable += 1;
            }
            last_complete = Some(seq);
            event(Event::Cycle(data));
            if stable >= 10 {
                break;
            }
        }
    }
    let hz = (clocks - 1) as f64 / last.duration_since(start).as_secs_f64();
    let lost = (usize::from(cycle) + 1) * axes.len() - axes.iter().map(|a| a.count).sum::<usize>();
    Ok(format!("{} 轴全部到位，完整反馈 {complete}/{} 周期，缺少 {lost} 轴帧，已发 {clocks} 拍，平均周期频率 {hz:.1} Hz，最大发送间隔 {:.2} ms",
        axes.len(),cycle+1,max_gap.as_secs_f64()*1000.0))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;
    use yfoc_can::CanError;

    #[derive(Default)]
    struct VirtualAxis {
        target: f64,
        second: Option<f64>,
        duration: u16,
        token: u16,
        ready: bool,
    }

    #[derive(Default)]
    struct TestBus {
        nodes: BTreeMap<u8, VirtualAxis>,
        frames: VecDeque<Frame>,
        refuse_arm: Option<u8>,
        drop_feedback: Option<u8>,
        starts: u32,
        stops: u32,
    }

    impl TestBus {
        fn group(n: u8) -> (Self, Plan) {
            let nodes = (1..=n).map(|i| (i, VirtualAxis::default())).collect();
            let axes = (1..=n)
                .map(|node| Axis {
                    node,
                    target: 0.025,
                    second: Some(-0.01),
                })
                .collect();
            (
                Self {
                    nodes,
                    ..Default::default()
                },
                Plan { axes, seconds: 0.2 },
            )
        }
    }

    impl CanBus for TestBus {
        fn send(&mut self, f: &Frame) -> Result<(), CanError> {
            let d = &f.data;
            let token = u16le(&d[2..]);
            if f.id == SYNC {
                let cycle = u16le(&d[4..]);
                if d[0] == 3 {
                    self.stops += 1;
                    for a in self.nodes.values_mut().filter(|a| a.token == token) {
                        a.ready = false;
                    }
                    return Ok(());
                }
                if d[0] == 1 {
                    assert!(self.nodes.values().all(|a| a.ready && a.token == token));
                    self.starts += 1;
                }
                for (&node, a) in self.nodes.iter().rev() {
                    if Some(node) == self.drop_feedback && cycle >= 4 {
                        continue;
                    }
                    let axis = Axis {
                        node,
                        target: a.target,
                        second: a.second,
                    };
                    let position = reference(&axis, 0.0, cycle, a.duration);
                    let mut data = [0; 8];
                    data[..2].copy_from_slice(&token.to_le_bytes());
                    data[2..4].copy_from_slice(&cycle.to_le_bytes());
                    data[4..6].copy_from_slice(&(mm(position) as i16).to_le_bytes());
                    let fb = Frame {
                        id: 0x540 + u32::from(node),
                        data,
                    };
                    let mut stale = fb;
                    stale.data[0] ^= 1;
                    self.frames.extend([stale, fb, fb]);
                }
            } else {
                let node = (f.id - 0x200) as u8;
                if let Some(a) = self.nodes.get_mut(&node) {
                    let mut data = [d[0], d[1], 0, 1, 0, 0, 0, 0];
                    match d[0] {
                        CAPS => {
                            if a.ready {
                                data[2] = 2;
                            }
                        }
                        LIMITS => {
                            data[4..6].copy_from_slice(&30000u16.to_le_bytes());
                            data[6..].copy_from_slice(&10000u16.to_le_bytes());
                        }
                        TARGET => {
                            a.target = f64::from(i32le(&d[4..])) * 0.001;
                            a.token = token;
                        }
                        SECOND => a.second = Some(f64::from(i32le(&d[4..])) * 0.001),
                        ARM => {
                            assert_eq!(token, a.token);
                            assert_eq!(d[6], node - 1);
                            if self.refuse_arm == Some(node) {
                                data[2] = 3;
                            } else {
                                a.ready = true;
                                a.duration = u16le(&d[4..]);
                            }
                        }
                        _ => panic!("unexpected group request"),
                    }
                    self.frames.push_back(Frame {
                        id: 0x500 + u32::from(node),
                        data,
                    });
                }
            }
            Ok(())
        }
        fn recv_timeout(&mut self, _: Duration) -> Result<Option<Frame>, CanError> {
            Ok(self.frames.pop_front())
        }
        fn shutdown(&mut self) -> Result<(), CanError> {
            Ok(())
        }
    }

    #[test]
    fn sixteen_axes_two_segments_join_on_cycle() {
        let (mut bus, plan) = TestBus::group(16);
        let mut last = None;
        let mut max_ref = f64::MIN;
        execute(&mut bus, &plan, &AtomicBool::new(false), |evt| {
            if let Event::Cycle(row) = evt {
                assert_eq!(row.len(), 16);
                assert!(row
                    .iter()
                    .enumerate()
                    .all(|(i, s)| usize::from(s.node) == i + 1
                        && s.cycle == row[0].cycle
                        && (s.position - s.reference).abs() <= 0.00051));
                assert!(last.is_none_or(|prev| prev < row[0].cycle));
                last = Some(row[0].cycle);
                max_ref = max_ref.max(row[0].reference);
            }
        })
        .unwrap();
        assert!(max_ref > 0.024 && last.unwrap() >= 89);
        assert_eq!((bus.starts, bus.stops), (1, 1));
    }

    #[test]
    fn missing_ready_aborts_before_start() {
        let (mut bus, plan) = TestBus::group(8);
        bus.refuse_arm = Some(8);
        assert!(execute(&mut bus, &plan, &AtomicBool::new(false), |_| {}).is_err());
        assert!(bus.starts == 0 && bus.stops > 0 && bus.nodes.values().all(|a| !a.ready));
    }

    #[test]
    fn lost_axis_stops_whole_group() {
        let (mut bus, plan) = TestBus::group(12);
        bus.drop_feedback = Some(9);
        let err = execute(&mut bus, &plan, &AtomicBool::new(false), |_| {}).unwrap_err();
        assert!(err.contains("节点 9 反馈超时"));
        assert!(bus.starts == 1 && bus.stops > 0 && bus.nodes.values().all(|a| !a.ready));
    }
    #[test]
    fn invalid_members_never_send_commands() {
        let (mut bus, mut plan) = TestBus::group(8);
        plan.axes[7].node = 1;
        assert!(execute(&mut bus, &plan, &AtomicBool::new(false), |_| {}).is_err());
        assert_eq!((bus.starts, bus.stops), (0, 0));
    }
}
