//! 多设备示波缓冲。运动帧和电流帧分开记到达时间，缺测通道不补 0。

use std::collections::{BTreeSet, HashMap};

use egui_plot::PlotPoint;

/// 一条曲线的种类，顺序与界面勾选一致。
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum Channel {
    /// 位置，rad。
    Pos = 0,
    /// 速度，rad/s。
    Vel = 1,
    /// 输出轴指令力矩，Nm。
    Volt = 2,
    /// 实测 Id，A。
    Id = 3,
    /// 实测 Iq，A。
    Iq = 4,
    /// Iq 指令，A。
    IqRef = 5,
    /// Vq，pu。
    Vq = 6,
}

impl Channel {
    /// 全部通道。
    pub const ALL: [Channel; 7] = [
        Channel::Pos,
        Channel::Vel,
        Channel::Volt,
        Channel::Id,
        Channel::Iq,
        Channel::IqRef,
        Channel::Vq,
    ];

    /// 中文名。
    pub fn title(self) -> &'static str {
        match self {
            Channel::Pos => "位置",
            Channel::Vel => "速度",
            Channel::Volt => "控制指令 Nm",
            Channel::Id => "实测 Id",
            Channel::Iq => "实测 Iq",
            Channel::IqRef => "Iq 指令",
            Channel::Vq => "Vq 输出",
        }
    }

    /// 单位。
    pub fn unit(self) -> &'static str {
        match self {
            Channel::Pos => "rad",
            Channel::Vel => "rad/s",
            Channel::Volt | Channel::Vq => "pu",
            Channel::Id | Channel::Iq | Channel::IqRef => "A",
        }
    }

    /// 基础颜色。多设备会在此颜色上稍作偏移。
    pub fn rgb(self) -> [u8; 3] {
        match self {
            Channel::Pos => [0xff, 0x4d, 0x5e],
            Channel::Vel => [0xf2, 0xc9, 0x4c],
            Channel::Volt => [0x5a, 0xc8, 0xfa],
            Channel::Id => [0xb3, 0x88, 0xff],
            Channel::Iq => [0x3d, 0xd6, 0x8c],
            Channel::IqRef => [0xff, 0x9f, 0x43],
            Channel::Vq => [0xe0, 0xe0, 0xdc],
        }
    }
}

const CAP: usize = 65536;

struct Ring {
    /// 相对启动的秒。
    t: Vec<f64>,
    /// 该通道自己的帧序号。
    frame: Vec<f64>,
    y: Vec<f32>,
    head: usize,
    len: usize,
}

impl Ring {
    fn new() -> Self {
        Self {
            t: vec![0.0; CAP],
            frame: vec![0.0; CAP],
            y: vec![0.0; CAP],
            head: 0,
            len: 0,
        }
    }

    fn push(&mut self, t: f64, frame: f64, y: f32) {
        self.t[self.head] = t;
        self.frame[self.head] = frame;
        self.y[self.head] = y;
        self.head = (self.head + 1) % CAP;
        self.len = (self.len + 1).min(CAP);
    }

    fn physical(&self, logical: usize) -> usize {
        let start = if self.len < CAP { 0 } else { self.head };
        (start + logical) % CAP
    }

    fn x_at(&self, logical: usize, time_axis: bool) -> f64 {
        let index = self.physical(logical);
        if time_axis { self.t[index] } else { self.frame[index] }
    }

    /** @brief 第一个横坐标不小于 x 的逻辑下标。横轴随写入单调不减。 */
    fn lower_bound(&self, x: f64, time_axis: bool) -> usize {
        let (mut lo, mut hi) = (0, self.len);
        while lo < hi {
            let mid = (lo + hi) / 2;
            if self.x_at(mid, time_axis) < x { lo = mid + 1 } else { hi = mid }
        }
        lo
    }

    /**
     * @brief 只遍历 [x_min, x_max] 内的点，两端各多带一个点让曲线连到边框。
     */
    fn visit_range(&self, time_axis: bool, x_min: f64, x_max: f64, mut visit: impl FnMut(f64, f32)) {
        if self.len == 0 {
            return;
        }
        let first = self.lower_bound(x_min, time_axis).saturating_sub(1);
        let end = (self.lower_bound(x_max, time_axis) + 1).min(self.len);
        for logical in first..end {
            let index = self.physical(logical);
            let x = if time_axis { self.t[index] } else { self.frame[index] };
            visit(x, self.y[index]);
        }
    }
}

/// 给界面的一条已降采样曲线。
#[derive(Clone, Debug)]
pub struct PlotSeries {
    /// 图例，例如 `ID3 位置`。
    pub name: String,
    /// 颜色。
    pub color: [u8; 3],
    /// 横轴、纵轴。
    pub points: Vec<PlotPoint>,
}

/// 实时曲线的显示状态。
#[derive(Clone, Debug)]
pub struct ScopeView {
    /// 暂停后不再写入。
    pub paused: bool,
    /// 横轴跟着最新数据。
    pub follow: bool,
    /// `true` 为时间，`false` 为帧号。
    pub time_axis: bool,
    /// 时间窗，秒。
    pub window_s: f64,
    /// 帧窗。
    pub window_frames: f64,
    /// 各通道独立归一化到 0..1。
    pub normalize: bool,
    /// 跟随模式下自动纵轴。
    pub auto_y: bool,
    /// 通道开关。
    pub enabled: [bool; 7],
    /// 参与叠加的节点。
    pub shown: BTreeSet<u8>,
    /// 绘图区宽度，像素。
    pub width_px: f32,
    /// 用户平移后的横轴。跟随开启时忽略。
    pub manual_x: Option<(f64, f64)>,
}

impl Default for ScopeView {
    fn default() -> Self {
        let mut enabled = [false; 7];
        enabled[Channel::Pos as usize] = true;
        Self {
            paused: false,
            follow: true,
            time_axis: true,
            window_s: 5.0,
            window_frames: 800.0,
            normalize: false,
            auto_y: true,
            enabled,
            shown: BTreeSet::new(),
            width_px: 900.0,
            manual_x: None,
        }
    }
}

/// 每个节点、每个通道一条环形缓冲。
#[derive(Default)]
pub struct Scope {
    rings: HashMap<(u8, u8), Ring>,
    frames: HashMap<(u8, u8), u64>,
    view: ScopeView,
    t0_us: Option<u64>,
    freeze_s: f64,
    /** @brief 最近一个采样点的横轴时间，秒。没连上时跟随窗口停在这里。 */
    last_t: f64,
    samples: usize,
}

impl Scope {
    /// 当前视图开关。
    pub fn view(&self) -> &ScopeView {
        &self.view
    }

    /// 修改视图开关。
    pub fn view_mut(&mut self) -> &mut ScopeView {
        &mut self.view
    }

    /// 清空曲线，时间零点留到下一次采样。
    pub fn clear(&mut self) {
        self.rings.clear();
        self.frames.clear();
        self.t0_us = None;
        self.freeze_s = 0.0;
        self.last_t = 0.0;
        self.samples = 0;
    }

    /// 节点换号时丢掉旧曲线。
    pub fn forget(&mut self, node: u8) {
        self.rings.retain(|(id, _), _| *id != node);
        self.frames.retain(|(id, _), _| *id != node);
        self.view.shown.remove(&node);
    }

    /// 新设备默认勾选，用户仍可关掉。
    pub fn ensure_shown(&mut self, node: u8) {
        self.view.shown.insert(node);
    }

    /// 记录一个通道的一个点。暂停时丢弃。
    pub fn push(&mut self, node: u8, channel: Channel, now_us: u64, value: f64) {
        if self.view.paused || !value.is_finite() {
            return;
        }
        let t0 = *self.t0_us.get_or_insert(now_us);
        let t = (now_us.saturating_sub(t0)) as f64 / 1_000_000.0;
        let key = (node, channel as u8);
        let frame = {
            let slot = self.frames.entry(key).or_insert(0);
            let current = *slot;
            *slot += 1;
            current
        };
        self.rings
            .entry(key)
            .or_insert_with(Ring::new)
            .push(t, frame as f64, value as f32);
        self.last_t = t;
        self.samples = self.samples.saturating_add(1);
        self.view.shown.insert(node);
    }

    /// 暂停时冻结跟随窗口的右端。
    pub fn set_paused(&mut self, paused: bool, now_us: u64) {
        if paused && !self.view.paused {
            self.freeze_s = self.now_s(now_us);
        }
        self.view.paused = paused;
    }

    /// 已记录的采样点数。
    pub fn samples(&self) -> usize {
        self.samples
    }

    fn now_s(&self, now_us: u64) -> f64 {
        match self.t0_us {
            Some(t0) => (now_us.saturating_sub(t0)) as f64 / 1_000_000.0,
            None => 0.0,
        }
    }

    /**
     * @brief 跟随窗口的横轴范围。
     *
     * `live` 为假时停在最后一个采样点，不随墙上时间往前走。
     */
    pub fn follow_x(&self, now_us: u64, live: bool) -> (f64, f64) {
        if self.view.time_axis {
            let now = if !live {
                self.last_t
            } else if self.view.paused {
                self.freeze_s
            } else {
                self.now_s(now_us)
            };
            (now - self.view.window_s, now)
        } else {
            let mut latest = 0.0_f64;
            for ((node, channel), count) in &self.frames {
                if self.view.shown.contains(node) && self.view.enabled[*channel as usize] && *count > 0 {
                    latest = latest.max(*count as f64 - 1.0);
                }
            }
            (latest - self.view.window_frames + 1.0, latest)
        }
    }

    /// 按像素做峰值降采样。归一化只影响返回的点，不改缓冲。
    pub fn series(&self, x_min: f64, x_max: f64) -> Vec<PlotSeries> {
        let pixels = (self.view.width_px as usize).clamp(32, 4096);
        let mut out = Vec::new();
        for node in &self.view.shown {
            for channel in Channel::ALL {
                if !self.view.enabled[channel as usize] {
                    continue;
                }
                let Some(ring) = self.rings.get(&(*node, channel as u8)) else {
                    continue;
                };
                let mut raw = Vec::new();
                ring.visit_range(self.view.time_axis, x_min, x_max, |x, y| {
                    if y.is_finite() {
                        raw.push(PlotPoint::new(x, y as f64));
                    }
                });
                if raw.is_empty() {
                    continue;
                }
                if self.view.normalize {
                    let (lo, hi) = raw.iter().fold((f64::MAX, f64::MIN), |(lo, hi), p| {
                        (lo.min(p.y), hi.max(p.y))
                    });
                    let span = hi - lo;
                    for point in &mut raw {
                        point.y = if span < 1e-12 { 0.0 } else { (point.y - lo) / span };
                    }
                }
                out.push(PlotSeries {
                    name: format!("ID{node} {}", channel.title()),
                    color: tint(*node, channel.rgb()),
                    points: peak_downsample(raw, pixels),
                });
            }
        }
        out
    }

    /// 跟随且自动纵轴时的显示范围。
    pub fn y_range(&self, series: &[PlotSeries]) -> Option<(f64, f64)> {
        if !self.view.auto_y {
            return None;
        }
        let mut lo = f64::MAX;
        let mut hi = f64::MIN;
        for item in series {
            for point in &item.points {
                lo = lo.min(point.y);
                hi = hi.max(point.y);
            }
        }
        if !lo.is_finite() || !hi.is_finite() {
            return None;
        }
        if hi - lo < 1e-9 {
            let pad = if hi.abs() < 1e-9 { 0.1 } else { hi.abs() * 0.1 };
            Some((lo - pad, hi + pad))
        } else {
            let pad = (hi - lo) * 0.08;
            Some((lo - pad, hi + pad))
        }
    }

    /// 某节点是否已经有电流采样。测试用来确认没有伪造的 0。
    #[cfg(test)]
    pub fn has_channel(&self, node: u8, channel: Channel) -> bool {
        self.frames.get(&(node, channel as u8)).copied().unwrap_or(0) > 0
    }
}

fn tint(node: u8, rgb: [u8; 3]) -> [u8; 3] {
    let shift = (node as i16 * 17) % 41 - 20;
    rgb.map(|channel| (channel as i16 + shift).clamp(32, 255) as u8)
}

/**
 * @brief 每个像素桶保留最小和最大，曲线尖峰不会被抽掉。
 *
 * 输入按横轴有序，同一桶的点必然相邻，单遍即可完成。
 */
fn peak_downsample(points: Vec<PlotPoint>, pixels: usize) -> Vec<PlotPoint> {
    if points.len() <= pixels * 2 {
        return points;
    }
    let x0 = points[0].x;
    let span = (points[points.len() - 1].x - x0).max(1e-9);
    let scale = pixels as f64 / span;
    let mut out = Vec::with_capacity(pixels * 2);
    let flush = |out: &mut Vec<PlotPoint>, lo: PlotPoint, hi: PlotPoint| {
        let (a, b) = if lo.x <= hi.x { (lo, hi) } else { (hi, lo) };
        out.push(a);
        if a.x != b.x || a.y != b.y {
            out.push(b);
        }
    };
    let mut bucket = usize::MAX;
    let mut lo = points[0];
    let mut hi = points[0];
    for point in points {
        let index = (((point.x - x0) * scale) as usize).min(pixels - 1);
        if index != bucket {
            if bucket != usize::MAX {
                flush(&mut out, lo, hi);
            }
            bucket = index;
            lo = point;
            hi = point;
        } else if point.y < lo.y {
            lo = point;
        } else if point.y > hi.y {
            hi = point;
        }
    }
    flush(&mut out, lo, hi);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn motion_does_not_invent_current_zero() {
        let mut scope = Scope::default();
        scope.push(1, Channel::Pos, 1_000_000, 0.25);
        assert!(scope.has_channel(1, Channel::Pos));
        assert!(!scope.has_channel(1, Channel::Id));
        let (x0, x1) = scope.follow_x(1_000_000, true);
        let series = scope.series(x0 - 1.0, x1 + 1.0);
        assert!(series.iter().all(|item| !item.name.contains("Id")));
    }

    #[test]
    fn window_lookup_survives_wraparound() {
        let mut scope = Scope::default();
        for i in 0..(CAP as u64 + 5000) {
            scope.push(1, Channel::Pos, i * 1000, i as f64);
        }
        let series = scope.series(60.0, 61.0);
        let points = &series[0].points;
        assert!(points.first().unwrap().x <= 60.0 && points.last().unwrap().x >= 61.0);
        assert!(points.iter().all(|p| p.x >= 59.99 && p.x <= 61.01));
        assert!(points.windows(2).all(|w| w[0].x <= w[1].x));
    }

    #[test]
    fn downsample_keeps_spike() {
        let mut points: Vec<PlotPoint> = (0..10_000).map(|i| PlotPoint::new(i as f64, 0.0)).collect();
        points[4321].y = 9.0;
        let out = peak_downsample(points, 100);
        assert!(out.len() <= 200);
        assert!(out.iter().any(|p| p.y == 9.0));
    }
}
