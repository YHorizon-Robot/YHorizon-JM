//! 深色 egui 界面：顶栏连接与急停，总览表看全部节点，下方调试选中节点，底部示波。

use std::sync::Arc;
use std::time::Duration;

use eframe::egui::{
    self, Align, Align2, Color32, CornerRadius, FontId, Frame, Key, Layout, Margin, Rect, RichText, Sense, Stroke,
    TextStyle, Ui, UiBuilder, Vec2,
};
use egui_plot::{Corner, Legend, Line, Plot, PlotPoints};

use yfoc_can::{interface_names, is_implemented, list_channels, BITRATES};
use yfoc_protocol::{WARN_ABS_UNKNOWN, WARN_TEMP};

use crate::scope::Channel;
use crate::session::{cmds, Command, CyclicKind, DeviceSnap, ScopeSnap, Snapshot, Target, DEFAULT_FEEDBACK_HZ, DEFAULT_STATUS_HZ};
use crate::theme::{self, Brand, BG, CARD_HI, FIELD, GREEN, MUTED, PANEL, RED, RED_HI, STROKE, TEXT, YELLOW};
use crate::worker::HostLink;

mod flash_page;
mod commission_page;
mod multi_axis_page;

/** @brief 启动窗口。 */
pub fn run() -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1440.0, 920.0])
            .with_min_inner_size([1100.0, 700.0])
            .with_icon(Arc::new(theme::window_icon()))
            .with_title(crate::session::HOST_TITLE),
        ..Default::default()
    };
    eframe::run_native(
        "YHorizon-JM Motor Host",
        options,
        Box::new(|cc| {
            theme::install(&cc.egui_ctx);
            Ok(Box::new(HostApp::new(&cc.egui_ctx)))
        }),
    )
}

/** @brief 运行控制的输入值，数值直接以 f64 保存。 */
struct Drive {
    kind: CyclicKind,
    motion_pos: f64,
    motion_vel: f64,
    motion_ff: f64,
    vel: f64,
    pos: f64,
    pos_max: f64,
    amps: f64,
    rate: f64,
}

impl Default for Drive {
    fn default() -> Self {
        Self {
            kind: CyclicKind::Velocity,
            motion_pos: 0.0,
            motion_vel: 0.0,
            motion_ff: 0.0,
            vel: 0.0,
            pos: 0.0,
            pos_max: 20.0,
            amps: 0.0,
            rate: 200.0,
        }
    }
}

impl Drive {
    fn target(&self) -> Target {
        let rate = self.rate;
        match self.kind {
            CyclicKind::Motion => Target::Motion { pos: self.motion_pos, vel: self.motion_vel, ff: self.motion_ff, rate },
            CyclicKind::Velocity => Target::Velocity { vel: self.vel, rate },
            CyclicKind::Position => Target::Position { pos: self.pos, max_vel: self.pos_max, rate },
            CyclicKind::Current => Target::Current { amps: self.amps, rate },
        }
    }

    fn zero(&mut self) {
        self.motion_pos = 0.0;
        self.motion_vel = 0.0;
        self.motion_ff = 0.0;
        self.vel = 0.0;
        self.amps = 0.0;
    }
}

/** @brief 选中设备的参数编辑器，切换设备时整体重置。 */
struct Params {
    owner: Option<u8>,
    gains: [[f64; 3]; 3],
    seen: [Option<(f64, f64, f64)>; 3],
    /// 用户点了读取之后，下一帧读回才写入输入框。
    pull: [bool; 3],
    gains_asked: bool,
    seen_inner: Option<u8>,
    inner: u8,
    set_mode: u8,
    new_id: u8,
}

impl Default for Params {
    fn default() -> Self {
        Self {
            owner: None,
            gains: [
                [0.2, 0.0, 0.002],
                [0.015, 0.15, 0.0],
                [30.0, 0.2, 0.0],
            ],
            seen: [None; 3],
            pull: [false; 3],
            gains_asked: false,
            seen_inner: None,
            inner: cmds::INNER_LOOP_VOLTAGE,
            set_mode: cmds::MODE_POSITION,
            new_id: 2,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Page {
    Motor,
    Flash,
    Commission,
    MultiAxis,
}

struct HostApp {
    link: HostLink,
    page: Page,
    flash: flash_page::FlashPane,
    commission: commission_page::Pane,
    group: multi_axis_page::Pane,
    brand: Brand,
    interface: String,
    channel: String,
    channels: Vec<String>,
    bitrate: u32,
    query_id: u8,
    drive: Drive,
    params: Params,
    tab: usize,
    seen_reset: u64,
    status_hz: f64,
    current_hz: f64,
    feedback_hz: f64,
    show_log: bool,
    /** @brief 已发给窗口的标题，变化时才重发。 */
    title: String,
    /** @brief 本地先行的跟随开关，避免等快照回来时拖动被拉回。 */
    local_follow: Option<bool>,
    /** @brief 上次发出的手动横轴范围。 */
    sent_manual: Option<(f64, f64)>,
}

impl HostApp {
    fn new(ctx: &egui::Context) -> Self {
        let interface = interface_names().into_iter().next().unwrap_or_else(|| "slcan".into());
        let channels = list_channels(&interface);
        let channel = channels.first().cloned().unwrap_or_default();
        Self {
            link: HostLink::spawn(),
            page: Page::Motor,
            flash: flash_page::FlashPane::new(),
            commission: commission_page::Pane::default(),
            group: multi_axis_page::Pane::default(),
            brand: Brand::load(ctx),
            interface,
            channel,
            channels,
            bitrate: 1_000_000,
            query_id: 1,
            drive: Drive::default(),
            params: Params::default(),
            tab: 0,
            seen_reset: 0,
            status_hz: DEFAULT_STATUS_HZ,
            current_hz: 0.0,
            feedback_hz: DEFAULT_FEEDBACK_HZ,
            show_log: true,
            title: String::new(),
            local_follow: None,
            sent_manual: None,
        }
    }

    fn telemetry(&self) {
        self.link.command(Command::Telemetry { status_hz: self.status_hz, current_hz: self.current_hz, feedback_hz: self.feedback_hz });
    }
}

impl eframe::App for HostApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let snap = self.link.snapshot();
        let flashing = self.page == Page::Flash && self.flash.active();
        let refresh = if flashing { 80 } else if snap.connected { 16 } else { 500 };
        ctx.request_repaint_after(Duration::from_millis(refresh));
        if snap.title != self.title {
            self.title.clone_from(&snap.title);
            ctx.send_viewport_cmd(egui::ViewportCommand::Title(snap.title.clone()));
        }
        if snap.connected && ctx.input(|input| input.key_pressed(Key::Escape)) {
            self.link.command(Command::StopAll);
        }
        self.sync(&snap);
        self.header(ctx, &snap);
        let motor = self.page == Page::Motor;
        if motor {
            self.scope_panel(ctx, &snap);
            if self.show_log {
                self.log_panel(ctx, &snap);
            }
        }
        egui::CentralPanel::default()
            .frame(Frame::new().fill(BG).inner_margin(Margin::same(14)))
            .show(ctx, |ui| {
                egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
                    match self.page {
                        Page::Motor => {
                            self.overview(ui, &snap);
                            ui.add_space(12.0);
                            self.detail(ui, &snap);
                        }
                        Page::Flash => self.flash_screen(ui, snap.connected || self.link.can_in_use()),
                        Page::Commission => self.commission_screen(ui, &snap),
                        Page::MultiAxis => self.multi_axis_screen(ui, &snap),
                    }
                });
            });
        if let Some(error) = &snap.error {
            egui::Window::new(RichText::new("提示").font(theme::bold(15.0)))
                .anchor(Align2::CENTER_CENTER, [0.0, 0.0])
                .collapsible(false)
                .resizable(false)
                .frame(Frame::window(&ctx.style()).inner_margin(Margin::same(18)))
                .show(ctx, |ui| {
                    ui.set_max_width(420.0);
                    ui.label(error);
                    ui.add_space(8.0);
                    ui.with_layout(Layout::right_to_left(Align::Min), |ui| {
                        if theme::primary(ui, true, "知道了").clicked() {
                            self.link.ack_error();
                        }
                    });
                });
        }
    }
}

impl HostApp {
    /** @brief 把快照里的读回值同步进编辑器，只在读回值变化时覆盖。 */
    fn sync(&mut self, snap: &Snapshot) {
        if snap.input_reset != self.seen_reset {
            self.seen_reset = snap.input_reset;
            self.drive.zero();
        }
        let Some(dev) = selected(snap) else {
            self.params.owner = None;
            return;
        };
        if self.params.owner != Some(dev.id) {
            let keep = (self.params.set_mode, self.params.new_id);
            self.params = Params { owner: Some(dev.id), set_mode: keep.0, new_id: keep.1, ..Params::default() };
        }
        if dev.inner != self.params.seen_inner {
            self.params.seen_inner = dev.inner;
            if let Some(inner) = dev.inner {
                self.params.inner = inner;
            }
        }
        for mode in 0..3 {
            if let Some(gains) = &dev.gains[mode] {
                let values = (gains.kp, gains.ki, gains.kd);
                if self.params.seen[mode] != Some(values) {
                    self.params.seen[mode] = Some(values);
                    if self.params.pull[mode] {
                        self.params.gains[mode] = [gains.kp, gains.ki, gains.kd];
                        self.params.pull[mode] = false;
                    }
                }
            }
        }
    }

    fn header(&mut self, ctx: &egui::Context, snap: &Snapshot) {
        egui::TopBottomPanel::top("header")
            .show_separator_line(false)
            .frame(Frame::new().fill(PANEL).inner_margin(Margin::symmetric(18, 10)))
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    self.brand.show(ui, 30.0);
                    ui.add_space(4.0);
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 0.0;
                        ui.label(RichText::new("YHorizon-JM 电机上位机").font(theme::bold(15.5)).color(TEXT));
                        ui.label(RichText::new("YHorizon-Robot · 元驭机器人").size(11.0).color(MUTED));
                    });
                    ui.add_space(18.0);
                    let (rect, _) = ui.allocate_exact_size(Vec2::new(1.0, 34.0), Sense::hover());
                    ui.painter().rect_filled(rect, CornerRadius::ZERO, STROKE);
                    ui.add_space(10.0);
                    let bench = self.link.commission_busy();
                    ui.add_enabled_ui(!self.flash.programming() && !bench, |ui| {
                        if theme::chip(ui, self.page == Page::Motor, "电机", None).clicked() {
                            self.page = Page::Motor;
                        }
                    }).response.on_disabled_hover_text("等待当前任务结束");
                    ui.add_enabled_ui(!bench, |ui| {
                        if theme::chip(ui, self.page == Page::Flash, "烧录", None).clicked() {
                            self.page = Page::Flash;
                        }
                    });
                    ui.add_enabled_ui(!self.flash.programming() && !bench, |ui| {
                        if theme::chip(ui, self.page == Page::Commission, "校准与辨识", None).clicked() {
                            self.page = Page::Commission;
                        }
                        if theme::chip(ui, self.page == Page::MultiAxis, "多轴同步", None).clicked() {
                            self.page = Page::MultiAxis;
                        }
                    });
                    if self.page != Page::Flash {
                        ui.add_space(8.0);
                        let (rect, _) = ui.allocate_exact_size(Vec2::new(1.0, 34.0), Sense::hover());
                        ui.painter().rect_filled(rect, CornerRadius::ZERO, STROKE);
                        ui.add_space(10.0);
                        if self.page == Page::Motor { self.connection_controls(ui, snap); }
                    }
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        let stop = egui::Button::new(RichText::new("■ 全部停止  Esc").font(theme::bold(14.0)).color(Color32::WHITE))
                            .fill(RED)
                            .min_size(Vec2::new(132.0, 34.0));
                        if ui.add_enabled(snap.connected, stop).on_hover_text("停止全部循环、直接电流补发 0 A，再广播 DISABLE 关桥").clicked() {
                            self.link.command(Command::StopAll);
                        }
                        if self.page == Page::Motor && theme::chip(ui, self.show_log, "日志", None).clicked() {
                            self.show_log = !self.show_log;
                        }
                        status_pill(ui, snap);
                    });
                });
            });
        egui::TopBottomPanel::top("horizon")
            .show_separator_line(false)
            .exact_height(2.0)
            .frame(Frame::NONE)
            .show(ctx, |ui| theme::horizon(ui.painter(), ui.max_rect()));
        if snap.overload {
            egui::TopBottomPanel::top("overload")
                .show_separator_line(false)
                .frame(
                    Frame::new()
                        .fill(Color32::from_rgba_unmultiplied(0xF2, 0xC9, 0x4C, 24))
                        .inner_margin(Margin::symmetric(18, 6)),
                )
                .show(ctx, |ui| {
                    ui.label(
                        RichText::new(format!(
                            "⚠ 循环与轮询预计 {:.0} 帧/s，约占 1 Mbps 总线 {:.0}%，请降低频率或减少节点",
                            snap.load_est,
                            snap.load_est * 100.0 / 7000.0
                        ))
                        .color(YELLOW),
                    );
                });
        }
    }

    fn connection_controls(&mut self, ui: &mut Ui, snap: &Snapshot) {
        labeled(ui, "接口", |ui| {
            let previous = self.interface.clone();
            egui::ComboBox::from_id_salt("iface")
                .width(110.0)
                .selected_text(iface_label(&self.interface))
                .show_ui(ui, |ui| {
                    for name in interface_names() {
                        ui.selectable_value(&mut self.interface, name.clone(), iface_label(&name));
                    }
                });
            if self.interface != previous {
                self.channels = list_channels(&self.interface);
                if !self.channels.iter().any(|item| item == &self.channel) {
                    self.channel = self.channels.first().cloned().unwrap_or_default();
                }
            }
        });
        labeled(ui, "通道", |ui| {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = 4.0;
                egui::ComboBox::from_id_salt("chan")
                    .selected_text(&self.channel)
                    .width(170.0)
                    .show_ui(ui, |ui| {
                        for name in &self.channels {
                            ui.selectable_value(&mut self.channel, name.clone(), name);
                        }
                    });
                if ui.button("⟳").on_hover_text("重新扫描适配器").clicked() {
                    self.channels = list_channels(&self.interface);
                    if !self.channels.iter().any(|item| item == &self.channel) {
                        self.channel = self.channels.first().cloned().unwrap_or_default();
                    }
                }
            });
        });
        labeled(ui, "波特率", |ui| {
            egui::ComboBox::from_id_salt("baud")
                .width(96.0)
                .selected_text(baud_label(self.bitrate))
                .show_ui(ui, |ui| {
                    for baud in BITRATES {
                        ui.selectable_value(&mut self.bitrate, *baud, baud_label(*baud));
                    }
                });
        });
        ui.add_space(6.0);
        ui.vertical(|ui| {
            ui.add_space(15.0);
            ui.horizontal(|ui| {
                if !snap.connected {
                    if theme::primary(ui, !self.link.can_in_use(), "连接").clicked() {
                        self.link.connect(self.interface.clone(), self.channel.clone(), self.bitrate);
                        self.telemetry();
                        self.link.command(Command::Query(self.query_id));
                    }
                } else if ui.button("断开").clicked() {
                    self.link.disconnect();
                }
            });
        });
    }

    /** @brief 总线总览：所有节点一行一台，可直接停止或清故障。 */
    fn overview(&mut self, ui: &mut Ui, snap: &Snapshot) {
        theme::card().show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.horizontal_wrapped(|ui| {
                theme::section(ui, "总线设备");
                theme::badge(ui, &format!("{} 台", snap.devices.iter().filter(|d| d.online).count()), MUTED);
                ui.add_space(12.0);
                ui.label(RichText::new("节点").color(MUTED));
                ui.add(egui::DragValue::new(&mut self.query_id).range(1..=63).speed(0.2));
                if ui.add_enabled(snap.connected, egui::Button::new("查询")).clicked() {
                    self.link.command(Command::Query(self.query_id));
                }
                if ui.add_enabled(snap.connected, egui::Button::new("扫描 1~63")).clicked() {
                    self.link.command(Command::Scan);
                }
                ui.add_space(12.0);
                ui.label(RichText::new("状态轮询").color(MUTED));
                let status = ui.add(egui::DragValue::new(&mut self.status_hz).range(0.0..=50.0).speed(0.1).suffix(" Hz"));
                ui.label(RichText::new("电流轮询").color(MUTED));
                let current = ui.add(egui::DragValue::new(&mut self.current_hz).range(0.0..=500.0).speed(1.0).suffix(" Hz"));
                ui.label(RichText::new("空闲反馈").color(MUTED)).on_hover_text("没有在循环的节点按此频率请求 GET_FEEDBACK，手推电机也能看到位置；需要固件 1.3");
                let feedback = ui.add(egui::DragValue::new(&mut self.feedback_hz).range(0.0..=200.0).speed(0.5).suffix(" Hz"));
                if status.changed() || current.changed() || feedback.changed() {
                    self.telemetry();
                }
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    ui.label(RichText::new(format!("预计负载 {:.0} 帧/s", snap.load_est)).monospace().color(if snap.overload { YELLOW } else { MUTED }));
                });
            });
            ui.add_space(6.0);
            egui::ScrollArea::horizontal().id_salt("overview").show(ui, |ui| {
                let width = ui.available_width().max(COLUMNS.iter().map(|c| c.1).sum::<f32>() + 160.0);
                table_header(ui, width);
                if snap.devices.is_empty() {
                    ui.add_space(14.0);
                    ui.label(RichText::new("还没有设备。连接后会查询左上的节点号，也可以扫描整条总线。").color(MUTED));
                    ui.add_space(10.0);
                }
                for dev in &snap.devices {
                    match device_row(ui, dev, snap.selected == Some(dev.id), width) {
                        RowAction::None => {}
                        RowAction::Select => self.link.command(Command::Select(dev.id)),
                        RowAction::Stop => self.link.command(Command::On(dev.id, Box::new(Command::Stop))),
                        RowAction::ClearFault => {
                            self.link.command(Command::On(dev.id, Box::new(Command::Mgmt { cmd: cmds::CMD_CLEAR_FAULT })))
                        }
                    }
                }
            });
        });
    }

    /** @brief 选中节点的调试区：左侧运行控制，右侧参数页。 */
    fn detail(&mut self, ui: &mut Ui, snap: &Snapshot) {
        let Some(dev) = selected(snap) else { return };
        let gap = 12.0;
        let avail = ui.available_width();
        if avail < 980.0 {
            self.run_card(ui, dev);
            ui.add_space(gap);
            self.param_card(ui, dev, snap);
            return;
        }
        let left = ((avail - gap) * 0.44).floor();
        let right = avail - gap - left;
        ui.horizontal_top(|ui| {
            ui.spacing_mut().item_spacing.x = gap;
            ui.allocate_ui_with_layout(Vec2::new(left, 0.0), Layout::top_down(Align::Min), |ui| {
                ui.set_width(left);
                self.run_card(ui, dev);
            });
            ui.allocate_ui_with_layout(Vec2::new(right, 0.0), Layout::top_down(Align::Min), |ui| {
                ui.set_width(right);
                self.param_card(ui, dev, snap);
            });
        });
    }

    fn run_card(&mut self, ui: &mut Ui, dev: &DeviceSnap) {
        theme::card().show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.horizontal(|ui| {
                theme::section(ui, &format!("运行控制 · ID {}", dev.id));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| activity_badge(ui, dev));
            });
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                for (kind, name) in [
                    (CyclicKind::Velocity, "速度"),
                    (CyclicKind::Position, "位置"),
                    (CyclicKind::Motion, "运动 MIT"),
                    (CyclicKind::Current, "电流"),
                ] {
                    if theme::chip(ui, self.drive.kind == kind, name, None).clicked() {
                        self.drive.kind = kind;
                    }
                }
            });
            ui.add_space(8.0);
            let live = dev.cyclic_kind == Some(self.drive.kind);
            let mut changed = false;
            egui::Grid::new("drive").num_columns(2).spacing([14.0, 10.0]).show(ui, |ui| {
                let d = &mut self.drive;
                match d.kind {
                    CyclicKind::Motion => {
                        changed |= value(ui, "位置", &mut d.motion_pos, " rad", 0.01, -1e4..=1e4);
                        changed |= value(ui, "速度", &mut d.motion_vel, " rad/s", 0.05, -1e4..=1e4);
                        changed |= value(ui, "前馈", &mut d.motion_ff, " Nm", 0.01, -32.767..=32.767);
                    }
                    CyclicKind::Velocity => {
                        changed |= value(ui, "目标速度", &mut d.vel, " rad/s", 0.05, -1e4..=1e4);
                    }
                    CyclicKind::Position => {
                        changed |= value(ui, "目标位置", &mut d.pos, " rad", 0.01, -1e4..=1e4);
                        changed |= value(ui, "最大速度", &mut d.pos_max, " rad/s", 0.1, 0.0..=1e4);
                    }
                    CyclicKind::Current => {
                        changed |= value(ui, "电流指令", &mut d.amps, " A", 0.01, -100.0..=100.0);
                    }
                }
                changed |= value(ui, "循环频率", &mut d.rate, " Hz", 1.0, 1.0..=2000.0);
            });
            if changed && live {
                self.link.command(Command::Retarget(self.drive.target()));
            }
            ui.add_space(10.0);
            let block = drive_block(dev, self.drive.kind);
            let can = block.is_none();
            ui.horizontal_wrapped(|ui| {
                if ui.add_enabled(can, egui::Button::new("发送一次").min_size(Vec2::new(86.0, 30.0))).clicked() {
                    self.link.command(Command::Drive { target: self.drive.target(), cyclic: false });
                }
                let start = if live { "循环中 · 立即更新" } else { "开始循环" };
                if theme::primary(ui, can, start).clicked() {
                    self.link.command(Command::Drive { target: self.drive.target(), cyclic: true });
                }
                let stop = theme::danger(ui, dev.online, "停止").on_hover_text("停止循环；固件 1.3 起同时 DISABLE 关桥");
                if stop.clicked() {
                    self.link.command(Command::Stop);
                }
                if self.drive.kind == CyclicKind::Position {
                    if let Some(pos) = dev.pos {
                        if ui.button("填入当前位置").clicked() {
                            self.drive.pos = pos;
                            if live {
                                self.link.command(Command::Retarget(self.drive.target()));
                            }
                        }
                    }
                }
            });
            ui.add_space(6.0);
            if let Some(reason) = block {
                note(ui, &reason, YELLOW);
            } else {
                note(ui, &drive_hint(dev, self.drive.kind), MUTED);
            }
            if dev.online && !dev.control_ext {
                note(ui, "旧固件不支持 DISABLE：停止只是不再发送，电机会保持最后一帧目标。", YELLOW);
            }
            if self.drive.kind == CyclicKind::Motion {
                if let Some(g) = &dev.gains[cmds::MODE_MOTION as usize] {
                    if g.kp == 0.0 && g.kd == 0.0 {
                        note(ui, "Motion 增益 Kp/Kd 都是 0，位置和速度目标不会产生力矩，只有前馈生效。", YELLOW);
                    }
                }
            }
        });
    }

    fn param_card(&mut self, ui: &mut Ui, dev: &DeviceSnap, snap: &Snapshot) {
        theme::card().show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.horizontal(|ui| {
                self.tab = theme::tabs(ui, self.tab, &["增益", "电流环", "管理", "信息"]);
                if dev.busy {
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| theme::badge(ui, "等待 ACK", YELLOW));
                }
            });
            ui.add_space(10.0);
            match self.tab {
                0 => self.tab_gains(ui, dev),
                1 => self.tab_current(ui, dev, snap),
                2 => self.tab_mgmt(ui, dev),
                _ => tab_info(ui, dev),
            }
        });
    }

    /**
     * @brief 读取指定模式的增益，并在应答到达后填入输入框。
     *
     * 已经拿到的读回立即填入，避免数值没变时下一帧不再覆盖。
     */
    fn pull_gains(&mut self, dev: &DeviceSnap, mode: u8) {
        let index = mode as usize;
        self.params.pull[index] = true;
        if let Some(gains) = &dev.gains[index] {
            self.params.gains[index] = [gains.kp, gains.ki, gains.kd];
            self.params.pull[index] = false;
        }
        self.link.command(Command::GetGains { mode });
    }

    fn tab_gains(&mut self, ui: &mut Ui, dev: &DeviceSnap) {
        let ready = dev.online && !dev.busy;
        if ready && !self.params.gains_asked && dev.gains.iter().all(Option::is_none) {
            self.params.gains_asked = true;
            for mode in [cmds::MODE_MOTION, cmds::MODE_VELOCITY, cmds::MODE_POSITION] {
                self.link.command(Command::On(dev.id, Box::new(Command::GetGains { mode })));
            }
        }
        let rows: [(u8, &str, [bool; 3]); 3] = [
            (cmds::MODE_MOTION, "运控 Nm", [true, false, true]),
            (cmds::MODE_VELOCITY, "速度", [true, true, false]),
            (cmds::MODE_POSITION, "位置", [true, true, true]),
        ];
        egui::Grid::new("gains").num_columns(6).spacing([12.0, 10.0]).show(ui, |ui| {
            for title in ["参数组", "Kp", "Ki", "Kd", "设备当前值", ""] {
                ui.label(RichText::new(title).size(12.0).color(MUTED));
            }
            ui.end_row();
            for (mode, name, used) in rows {
                let current = dev.mode == Some(mode);
                let label = RichText::new(name).color(if current { TEXT } else { MUTED });
                ui.label(if current { label.strong() } else { label });
                let values = &mut self.params.gains[mode as usize];
                for (slot, on) in values.iter_mut().zip(used) {
                    ui.add_enabled(on, egui::DragValue::new(slot).speed(0.01).max_decimals(6).update_while_editing(false));
                }
                let readback = dev.gains[mode as usize]
                    .as_ref()
                    .map(|g| format!("{} / {} / {}", fmt_num(g.kp), fmt_num(g.ki), fmt_num(g.kd)))
                    .unwrap_or_else(|| "未读取".into());
                ui.label(RichText::new(readback).monospace().size(12.0).color(MUTED));
                ui.horizontal(|ui| {
                    if ui.add_enabled(ready, egui::Button::new("读取")).clicked() {
                        self.pull_gains(dev, mode);
                    }
                    if theme::primary(ui, ready, "写入").clicked() {
                        let [kp, ki, kd] = self.params.gains[mode as usize];
                        self.link.command(Command::Gains { mode, kp, ki, kd });
                        self.link.command(Command::GetGains { mode });
                    }
                });
                ui.end_row();
            }
        });
        ui.add_space(8.0);
        ui.horizontal(|ui| {
            if ui.add_enabled(ready, egui::Button::new("全部读取")).clicked() {
                for mode in [cmds::MODE_MOTION, cmds::MODE_VELOCITY, cmds::MODE_POSITION] {
                    self.pull_gains(dev, mode);
                }
            }
            if ui.add_enabled(ready, egui::Button::new("保存到 Flash")).clicked() {
                self.link.command(Command::SaveParams);
            }
        });
        note(ui, "输入框是推荐初值：运控 Kp 0.2、Kd 0.002 Nm，速度 0.015 / 0.15，位置 Kp 30、Ki 0.2。点读取才用设备当前值覆盖。写入只改 RAM。保存到 Flash 只包含速度 Kp/Ki 与位置 Kp/Ki/Kd。", MUTED);
    }

    fn tab_current(&mut self, ui: &mut Ui, dev: &DeviceSnap, snap: &Snapshot) {
        if !dev.supports_current {
            note(ui, "该设备固件不支持电流协议，没有电流环页。", MUTED);
            return;
        }
        let idle = dev.current_idle;
        ui.horizontal(|ui| {
            ui.label(RichText::new("内环").color(MUTED));
            ui.add_enabled_ui(idle, |ui| {
                for (mode, name) in [(cmds::INNER_LOOP_VOLTAGE, "电压环"), (cmds::INNER_LOOP_CURRENT, "电流环")] {
                    if theme::chip(ui, self.params.inner == mode, name, None).clicked() {
                        self.params.inner = mode;
                    }
                }
            });
            let differs = dev.inner != Some(self.params.inner);
            if theme::primary(ui, idle && differs, "切换").on_hover_text("先停止本节点，成功后清零指令输入并重新读配置").clicked() {
                self.link.command(Command::InnerLoop { mode: self.params.inner });
            }
            if ui.add_enabled(idle, egui::Button::new("刷新配置")).clicked() {
                self.link.command(Command::RefreshCurrent);
            }
            ui.label(RichText::new(format!("当前：{}", dev.inner_text)).color(MUTED));
        });
        ui.add_space(8.0);
        Frame::new()
            .fill(FIELD)
            .corner_radius(CornerRadius::same(8))
            .inner_margin(Margin::symmetric(12, 8))
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                ui.label(RichText::new(&dev.config_label).color(TEXT));
            });
        ui.add_space(8.0);
        let state = dev.current.as_ref();
        let items = [
            ("Id", state.map(|s| s.id), "A"),
            ("Iq", state.map(|s| s.iq), "A"),
            ("Iq_ref", state.map(|s| s.iq_ref), "A"),
            ("Vq", state.map(|s| s.vq), "pu"),
        ];
        ui.horizontal(|ui| {
            for (name, val, unit) in items {
                metric(ui, name, &opt(val, 3), unit);
            }
        });
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            if ui.add_enabled(dev.online, egui::Button::new("读一次")).clicked() {
                self.link.command(Command::Mgmt { cmd: cmds::CMD_GET_CURRENT_STATE });
            }
            if snap.current_hz <= 0.0 {
                note(ui, "电流轮询已关闭，实测值只在点「读一次」时更新。可在总览栏打开电流轮询。", YELLOW);
            } else {
                note(ui, &format!("每 {:.0} ms 轮询一次", 1000.0 / snap.current_hz), MUTED);
            }
        });
    }

    fn tab_mgmt(&mut self, ui: &mut Ui, dev: &DeviceSnap) {
        let ready = !dev.busy;
        ui.horizontal_wrapped(|ui| {
            for (name, cmd, tip) in [
                ("清除故障", cmds::CMD_CLEAR_FAULT, "CLEAR_FAULT"),
                ("设零点", cmds::CMD_SET_ZERO, "SET_ZERO：当前位置记为 0"),
                ("开始校准", cmds::CMD_START_CALI, "START_CALI：会先停止本节点循环"),
                ("读状态", cmds::CMD_GET_STATUS, "GET_STATUS"),
                ("关桥 DISABLE", cmds::CMD_DISABLE, "停止循环并关桥，清除保持的目标；新实时命令自动恢复"),
                ("读反馈", cmds::CMD_GET_FEEDBACK, "GET_FEEDBACK：回一帧位置/速度/输出轴力矩"),
            ] {
                if cmd == cmds::CMD_START_CALI && dev.supports_commission {
                    if ui.add_enabled(ready, egui::Button::new("校准与辨识")).clicked() {
                        self.commission.select_node(dev.id);
                        self.page = Page::Commission;
                    }
                    continue;
                }
                if ui.add_enabled(ready, egui::Button::new(name)).on_hover_text(tip).clicked() {
                    self.link.command(Command::Mgmt { cmd });
                }
            }
        });
        ui.add_space(10.0);
        egui::Grid::new("mgmt").num_columns(2).spacing([16.0, 10.0]).show(ui, |ui| {
            ui.label(RichText::new("外环模式").color(MUTED));
            ui.horizontal(|ui| {
                for (mode, name) in [(cmds::MODE_MOTION, "Motion"), (cmds::MODE_VELOCITY, "速度"), (cmds::MODE_POSITION, "位置")] {
                    if theme::chip(ui, self.params.set_mode == mode, name, None).clicked() {
                        self.params.set_mode = mode;
                    }
                }
                if theme::primary(ui, ready, "设置").clicked() {
                    self.link.command(Command::SetMode { mode: self.params.set_mode });
                }
                ui.label(RichText::new(format!("当前：{}", dev.mode_name)).color(MUTED));
            });
            ui.end_row();
            ui.label(RichText::new("节点号").color(MUTED));
            ui.horizontal(|ui| {
                ui.add(egui::DragValue::new(&mut self.params.new_id).range(1..=63).speed(0.2));
                if theme::primary(ui, ready && self.params.new_id != dev.id, "改为该 ID").clicked() {
                    self.link.command(Command::SetNodeId { new_id: self.params.new_id });
                }
            });
            ui.end_row();
        });
        note(
            ui,
            "运行控制会按目标自动切换外环，这里的外环设置用于手动调试。改节点号成功后电机复位，上位机自动改用新 ID。",
            MUTED,
        );
    }

    fn scope_panel(&mut self, ctx: &egui::Context, snap: &Snapshot) {
        let scope = &snap.scope;
        if self.local_follow == Some(scope.view.follow) {
            self.local_follow = None;
        }
        let follow = self.local_follow.unwrap_or(scope.view.follow);
        egui::TopBottomPanel::bottom("scope")
            .resizable(true)
            .default_height(340.0)
            .height_range(220.0..=1000.0)
            .show_separator_line(false)
            .frame(Frame::new().fill(BG).inner_margin(Margin { left: 14, right: 14, top: 10, bottom: 14 }))
            .show(ctx, |ui| {
                theme::card().inner_margin(Margin::same(12)).show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    ui.set_min_height(ui.available_height());
                    self.scope_toolbar(ui, snap, follow);
                    self.scope_plot(ui, scope, follow);
                });
            });
    }

    fn scope_toolbar(&mut self, ui: &mut Ui, snap: &Snapshot, follow: bool) {
        let scope = &snap.scope;
        ui.horizontal_wrapped(|ui| {
            theme::section(ui, "实时示波");
            ui.add_space(8.0);
            if theme::chip(ui, scope.view.time_axis, "时间", None).clicked() {
                self.link.command(Command::ScopeTimeAxis(true));
            }
            if theme::chip(ui, !scope.view.time_axis, "按帧", None).clicked() {
                self.link.command(Command::ScopeTimeAxis(false));
            }
            if scope.view.time_axis {
                let mut window = scope.view.window_s;
                let slider = egui::Slider::new(&mut window, 0.2..=60.0).logarithmic(true).suffix(" s").max_decimals(1);
                if ui.add(slider).changed() {
                    self.link.command(Command::ScopeWindowSecs(window));
                }
            } else {
                let mut window = scope.view.window_frames;
                let slider = egui::Slider::new(&mut window, 50.0..=20_000.0).logarithmic(true).suffix(" 帧").max_decimals(0);
                if ui.add(slider).changed() {
                    self.link.command(Command::ScopeWindowFrames(window));
                }
            }
            ui.add_space(6.0);
            if theme::chip(ui, follow, "跟随", None).clicked() {
                self.set_follow(!follow);
            }
            if theme::chip(ui, scope.view.paused, "暂停", Some(YELLOW)).clicked() {
                self.link.command(Command::ScopePause(!scope.view.paused));
            }
            if theme::chip(ui, scope.view.normalize, "归一化", None).clicked() {
                self.link.command(Command::ScopeNormalize(!scope.view.normalize));
            }
            if theme::chip(ui, scope.view.auto_y, "自动纵轴", None).clicked() {
                self.link.command(Command::ScopeAutoY(!scope.view.auto_y));
            }
            ui.add_space(6.0);
            if ui.button("清空").clicked() {
                self.link.command(Command::ScopeClear);
            }
            ui.label(RichText::new(format!("{} 点", scope.samples)).color(MUTED));
        });
        ui.horizontal_wrapped(|ui| {
            ui.spacing_mut().item_spacing = Vec2::new(6.0, 6.0);
            if !snap.devices.is_empty() {
                ui.label(RichText::new("设备").color(MUTED));
                for dev in &snap.devices {
                    let on = scope.view.shown.contains(&dev.id);
                    if theme::chip(ui, on, &format!("ID {}", dev.id), None).clicked() {
                        self.link.command(Command::ScopeDevice { id: dev.id, on: !on });
                    }
                }
                ui.add_space(10.0);
            }
            ui.label(RichText::new("通道").color(MUTED));
            for channel in Channel::ALL {
                let on = scope.view.enabled[channel as usize];
                let [r, g, b] = channel.rgb();
                let text = format!("{} {}", channel.title(), channel.unit());
                if theme::chip(ui, on, &text, Some(Color32::from_rgb(r, g, b))).clicked() {
                    self.link.command(Command::ScopeChannel { index: channel as usize, on: !on });
                }
            }
            let wants_current = Channel::ALL.iter().any(|c| is_current_channel(*c) && scope.view.enabled[*c as usize]);
            if wants_current && snap.current_hz <= 0.0 && snap.connected {
                ui.add_space(8.0);
                ui.label(RichText::new("电流通道需要电流轮询").color(YELLOW));
                if ui.button("开启 100 Hz").clicked() {
                    self.current_hz = 100.0;
                    self.telemetry();
                }
            }
        });
        ui.add_space(4.0);
    }

    fn scope_plot(&mut self, ui: &mut Ui, scope: &ScopeSnap, follow: bool) {
        let width = ui.available_width();
        if (width - scope.view.width_px).abs() > 2.0 {
            self.link.command(Command::ScopeWidth(width));
        }
        let ctrl = ui.input(|input| input.modifiers.command || input.modifiers.ctrl);
        let plot = Plot::new("scope")
            .legend(Legend::default().position(Corner::LeftTop).background_alpha(0.75))
            .allow_zoom(egui::Vec2b::new(!ctrl, ctrl))
            .allow_drag(true)
            .allow_double_click_reset(false)
            .height((ui.available_height() - 40.0).max(80.0))
            .x_axis_label(if scope.view.time_axis { "t (s)" } else { "frame" })
            .y_axis_label(if scope.view.normalize { "归一化" } else { "值" });
        let response = plot.show(ui, |plot_ui| {
            if follow {
                if let Some((x0, x1)) = scope.x_range {
                    let (y0, y1) = scope.y_range.unwrap_or_else(|| {
                        let bounds = plot_ui.plot_bounds();
                        (bounds.min()[1], bounds.max()[1])
                    });
                    plot_ui.set_plot_bounds(egui_plot::PlotBounds::from_min_max([x0, y0], [x1, y1]));
                }
            }
            for series in &scope.series {
                let [r, g, b] = series.color;
                let line = Line::new(&series.name, PlotPoints::Borrowed(&series.points))
                    .color(Color32::from_rgb(r, g, b))
                    .width(1.6_f32);
                plot_ui.line(line);
            }
            plot_ui.pointer_coordinate().map(|pointer| cursor_text(pointer, scope))
        });
        if response.response.double_clicked() {
            self.set_follow(true);
        } else {
            if response.response.dragged() && follow {
                self.set_follow(false);
            }
            if !self.local_follow.unwrap_or(scope.view.follow) {
                let bounds = response.transform.bounds();
                let range = (bounds.min()[0], bounds.max()[0]);
                let changed = self.sent_manual.is_none_or(|(a, b)| {
                    let tol = (range.1 - range.0).abs() * 1e-4;
                    (a - range.0).abs() > tol || (b - range.1).abs() > tol
                });
                if changed && range.1 > range.0 {
                    self.sent_manual = Some(range);
                    self.link.command(Command::ScopeManualX { min: range.0, max: range.1 });
                }
            }
        }
        ui.horizontal(|ui| {
            let text = response.inner.unwrap_or_else(|| "光标 —".into());
            ui.add(egui::Label::new(RichText::new(text).monospace().color(TEXT)).truncate());
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.label(RichText::new("滚轮缩放横轴 · Ctrl+滚轮缩放纵轴 · 拖动平移 · 双击恢复跟随").size(11.5).color(MUTED));
            });
        });
    }

    fn set_follow(&mut self, on: bool) {
        self.local_follow = Some(on);
        self.sent_manual = None;
        self.link.command(Command::ScopeFollow(on));
    }

    fn log_panel(&self, ctx: &egui::Context, snap: &Snapshot) {
        egui::SidePanel::right("log")
            .resizable(true)
            .default_width(320.0)
            .width_range(200.0..=640.0)
            .frame(Frame::new().fill(PANEL).inner_margin(Margin::same(14)))
            .show(ctx, |ui| {
                theme::section(ui, "日志");
                ui.add_space(4.0);
                Frame::new()
                    .fill(Color32::from_rgb(0x0B, 0x0B, 0x0D))
                    .corner_radius(CornerRadius::same(8))
                    .inner_margin(Margin::same(8))
                    .show(ui, |ui| {
                        let row = ui.text_style_height(&TextStyle::Monospace);
                        ui.spacing_mut().item_spacing.y = 3.0;
                        egui::ScrollArea::vertical()
                            .auto_shrink([false, false])
                            .stick_to_bottom(true)
                            .show_rows(ui, row, snap.logs.len(), |ui, range| {
                                for line in &snap.logs[range] {
                                    let text = RichText::new(line).monospace().color(log_color(line));
                                    ui.add(egui::Label::new(text).truncate());
                                }
                            });
                    });
            });
    }
}

/** @brief 总览表的列：标题与宽度，最后的操作列按剩余宽度伸展。 */
const COLUMNS: [(&str, f32); 14] = [
    ("ID", 56.0),
    ("设备", 150.0),
    ("状态", 96.0),
    ("故障", 120.0),
    ("外环", 76.0),
    ("内环", 56.0),
    ("位置 rad", 88.0),
    ("速度 rad/s", 88.0),
    ("指令 Nm", 76.0),
    ("Iq A", 72.0),
    ("Vbus", 68.0),
    ("温度", 64.0),
    ("反馈", 64.0),
    ("活动", 136.0),
];

const ROW_H: f32 = 32.0;

enum RowAction {
    None,
    Select,
    Stop,
    ClearFault,
}

fn table_header(ui: &mut Ui, width: f32) {
    let (rect, _) = ui.allocate_exact_size(Vec2::new(width, 24.0), Sense::hover());
    let painter = ui.painter_at(rect);
    let mut x = rect.left() + 12.0;
    for (title, w) in COLUMNS {
        painter.text(egui::pos2(x, rect.center().y), Align2::LEFT_CENTER, title, FontId::proportional(12.0), MUTED);
        x += w;
    }
    painter.text(egui::pos2(x, rect.center().y), Align2::LEFT_CENTER, "操作", FontId::proportional(12.0), MUTED);
    painter.hline(rect.x_range(), rect.bottom(), Stroke::new(1.0_f32, STROKE));
}

/** @brief 一行设备。整行可点选，行尾按钮直接作用于该节点。 */
fn device_row(ui: &mut Ui, dev: &DeviceSnap, selected: bool, width: f32) -> RowAction {
    let (rect, row) = ui.allocate_exact_size(Vec2::new(width, ROW_H), Sense::click());
    let painter = ui.painter_at(rect);
    if selected {
        painter.rect_filled(rect, CornerRadius::same(6), CARD_HI);
        painter.rect_filled(Rect::from_min_size(rect.min + Vec2::new(0.0, 6.0), Vec2::new(3.0, ROW_H - 12.0)), CornerRadius::same(2), RED);
    } else if row.hovered() {
        painter.rect_filled(rect, CornerRadius::same(6), Color32::from_rgb(0x20, 0x20, 0x24));
    }
    let offline = !dev.online || dev.stale;
    let dim = |c: Color32| if offline { MUTED } else { c };
    let state_color = if offline { MUTED } else { theme::state_color(&dev.state_name) };
    let (fault_text, fault_color) = if dev.fault != 0 {
        (dev.fault_name.clone(), RED_HI)
    } else if dev.warn != 0 {
        (warn_text(dev.warn), YELLOW)
    } else {
        ("无".to_string(), MUTED)
    };
    let board = if dev.board_name.is_empty() { "—".to_string() } else { format!("{} · {}", dev.board_name, dev.fw) };
    let iq = dev.current.as_ref().map(|s| s.iq);
    let cells: [(String, Color32, bool); 14] = [
        (format!("ID {}", dev.id), dim(TEXT), false),
        (board, MUTED, false),
        (if offline && dev.online { "失联".into() } else { dev.state_name.clone() }, state_color, false),
        (fault_text, fault_color, false),
        (dev.mode_name.clone(), dim(TEXT), false),
        (dev.inner_text.clone(), dim(TEXT), false),
        (opt(dev.pos, 4), dim(TEXT), true),
        (opt(dev.vel, 3), dim(TEXT), true),
        (opt(dev.torque, 3), dim(TEXT), true),
        (opt(iq, 3), dim(TEXT), true),
        (dev.vbus.map(|v| format!("{v:.1} V")).unwrap_or_else(|| "—".into()), dim(TEXT), true),
        (dev.temp.map(|t| format!("{t:.1} °C")).unwrap_or_else(|| "—".into()), dim(TEXT), true),
        (format!("{:.0} Hz", dev.fb_hz), if dev.fb_hz > 0.0 { dim(TEXT) } else { MUTED }, true),
        (activity_text(dev), activity_color(dev), false),
    ];
    let mut x = rect.left() + 12.0;
    let y = rect.center().y;
    for ((text, color, mono), (_, w)) in cells.iter().zip(COLUMNS) {
        let cell = Rect::from_min_max(egui::pos2(x, rect.top()), egui::pos2(x + w - 8.0, rect.bottom()));
        let font = if *mono { FontId::monospace(13.0) } else { FontId::proportional(13.5) };
        if x == rect.left() + 12.0 {
            painter.circle_filled(egui::pos2(x + 4.0, y), 4.0, state_color);
            painter.with_clip_rect(cell).text(egui::pos2(x + 14.0, y), Align2::LEFT_CENTER, text, theme::bold(14.0), *color);
        } else {
            painter.with_clip_rect(cell).text(egui::pos2(x, y), Align2::LEFT_CENTER, text, font, *color);
        }
        x += w;
    }
    let actions = Rect::from_min_max(egui::pos2(x, rect.top() + 3.0), egui::pos2(rect.right() - 6.0, rect.bottom() - 3.0));
    let mut action = RowAction::None;
    let mut child = ui.new_child(UiBuilder::new().max_rect(actions).layout(Layout::left_to_right(Align::Center)));
    child.spacing_mut().item_spacing.x = 6.0;
    if dev.cyclic || dev.direct_current || (dev.online && dev.control_ext) {
        let stop = egui::Button::new(RichText::new("停止").color(RED_HI)).stroke(Stroke::new(1.0_f32, RED)).small();
        if child.add(stop).clicked() {
            action = RowAction::Stop;
        }
    }
    if dev.fault != 0 || dev.state_name == "FAULT" {
        if child.add(egui::Button::new("清故障").small()).clicked() {
            action = RowAction::ClearFault;
        }
    }
    if matches!(action, RowAction::None) && row.clicked() {
        action = RowAction::Select;
    }
    row.on_hover_cursor(egui::CursorIcon::PointingHand);
    action
}

fn activity_text(dev: &DeviceSnap) -> String {
    if dev.busy {
        "等待 ACK".into()
    } else if dev.direct_current && dev.cyclic {
        format!("电流循环 {:.0} Hz", dev.cyclic_hz)
    } else if dev.direct_current {
        "直接电流".into()
    } else if let Some(kind) = dev.cyclic_kind {
        format!("{} {:.0} Hz", kind_name(kind), dev.cyclic_hz)
    } else {
        "空闲".into()
    }
}

/** @brief 设备列表里的警告短文字。 */
fn warn_text(warn: u8) -> String {
    let mut parts: Vec<&str> = Vec::new();
    if warn & WARN_TEMP != 0 {
        parts.push("过温");
    }
    if warn & WARN_ABS_UNKNOWN != 0 {
        parts.push("多圈未建立");
    }
    if parts.is_empty() { format!("警告 0x{warn:02X}") } else { parts.join("、") }
}

/** @brief 设备信息页里的警告说明，带处理办法。 */
fn warn_detail(warn: u8) -> String {
    let mut parts: Vec<&str> = Vec::new();
    if warn & WARN_TEMP != 0 {
        parts.push("板温警告：达到告警阈值，继续升高会过温故障关桥");
    }
    if warn & WARN_ABS_UNKNOWN != 0 {
        parts.push("多圈位置未建立：上电定圈失败，位置只在零点所在的那一圈内有效，设零点会被拒绝；停止后游标解码一致会自动补上，否则请重新校准");
    }
    if parts.is_empty() { format!("0x{warn:02X}") } else { parts.join("\n") }
}

fn activity_color(dev: &DeviceSnap) -> Color32 {
    if dev.busy || dev.direct_current {
        YELLOW
    } else if dev.cyclic {
        GREEN
    } else {
        MUTED
    }
}

fn activity_badge(ui: &mut Ui, dev: &DeviceSnap) {
    theme::badge(ui, &activity_text(dev), activity_color(dev));
}

fn kind_name(kind: CyclicKind) -> &'static str {
    match kind {
        CyclicKind::Motion => "Motion",
        CyclicKind::Velocity => "速度",
        CyclicKind::Position => "位置",
        CyclicKind::Current => "电流",
    }
}

/** @brief 当前选项不能下发的原因；可以下发时为空。 */
fn drive_block(dev: &DeviceSnap, kind: CyclicKind) -> Option<String> {
    if !dev.online {
        return Some("设备还没有应答，请先查询或扫描。".into());
    }
    if dev.busy {
        return Some("正在等待上一条设置的 ACK。".into());
    }
    if kind != CyclicKind::Current {
        return None;
    }
    if !dev.supports_current {
        Some("该固件不支持直接电流。".into())
    } else if dev.inner != Some(cmds::INNER_LOOP_CURRENT) {
        Some("内环不是电流环，请先在右侧「电流环」页切换。".into())
    } else if !dev.config.has_scale() {
        Some("电流配置未读到，请在「电流环」页点刷新配置。".into())
    } else {
        None
    }
}

fn drive_hint(dev: &DeviceSnap, kind: CyclicKind) -> String {
    let want = match kind {
        CyclicKind::Velocity => cmds::MODE_VELOCITY,
        CyclicKind::Position => cmds::MODE_POSITION,
        CyclicKind::Motion | CyclicKind::Current => cmds::MODE_MOTION,
    };
    let mode = match dev.mode {
        Some(mode) if mode == want => "外环已匹配，直接发送。".to_string(),
        Some(_) => format!("当前外环 {}，发送前会自动切换并等待 ACK。", dev.mode_name),
        None => "外环未知，发送前会先设置外环并等待 ACK。".to_string(),
    };
    let extra = match kind {
        CyclicKind::Current => " 直接电流会先清零 Motion 增益；停止或断开时补发 0 A。",
        _ => " 循环中修改数值（回车或拖动）立即替换正在发送的目标。",
    };
    format!("{mode}{extra}")
}

fn tab_info(ui: &mut Ui, dev: &DeviceSnap) {
    egui::Grid::new("info").num_columns(2).spacing([24.0, 8.0]).show(ui, |ui| {
        let rows = [
            ("板卡", if dev.board_name.is_empty() { "—".into() } else { dev.board_name.clone() }),
            ("固件", if dev.fw.is_empty() { "—".into() } else { dev.fw.clone() }),
            ("能力", if dev.caps_text.is_empty() { "—".into() } else { dev.caps_text.clone() }),
            ("电流协议", if dev.supports_current { "支持".into() } else { "不支持".into() }),
            ("运控扩展", if dev.control_ext { "支持 DISABLE / GET_FEEDBACK / 故障上报".into() } else { "不支持（旧固件，停止后保持最后目标）".into() }),
            ("状态", format!("{}（故障 {}）", dev.state_name, dev.fault_name)),
            ("警告", if dev.warn == 0 { "无".into() } else { warn_detail(dev.warn) }),
            ("外环 / 内环", format!("{} / {}", dev.mode_name, dev.inner_text)),
            ("母线电压", dev.vbus.map(|v| format!("{v:.2} V")).unwrap_or_else(|| "—".into())),
            ("板温", dev.temp.map(|t| format!("{t:.1} °C")).unwrap_or_else(|| "未安装".into())),
            ("反馈频率", format!("{:.0} Hz", dev.fb_hz)),
            ("最近上行", format!("{:.1} s 前", dev.age_s)),
        ];
        for (name, text) in rows {
            ui.label(RichText::new(name).color(MUTED));
            ui.label(RichText::new(text).color(TEXT));
            ui.end_row();
        }
    });
}

/** @brief 顶栏右侧的连接状态胶囊。 */
fn status_pill(ui: &mut Ui, snap: &Snapshot) {
    Frame::new()
        .fill(FIELD)
        .stroke(Stroke::new(1.0_f32, STROKE))
        .corner_radius(CornerRadius::same(16))
        .inner_margin(Margin::symmetric(12, 5))
        .show(ui, |ui| {
            ui.horizontal(|ui| {
                theme::dot(ui, if snap.connected { GREEN } else { MUTED });
                ui.label(RichText::new(&snap.link_text).color(if snap.connected { TEXT } else { MUTED }));
                if snap.connected {
                    ui.label(RichText::new(format!("{:.0} 帧/s", snap.bus_fps)).monospace().color(MUTED));
                }
            });
        });
}

/** @brief 小号指标：上方名称，下方数值。 */
fn metric(ui: &mut Ui, name: &str, text: &str, unit: &str) {
    Frame::new()
        .fill(FIELD)
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::symmetric(12, 6))
        .show(ui, |ui| {
            ui.set_min_width(96.0);
            ui.vertical(|ui| {
                ui.spacing_mut().item_spacing.y = 1.0;
                ui.label(RichText::new(format!("{name} {unit}")).size(11.5).color(MUTED));
                ui.label(RichText::new(text).font(theme::bold(16.0)).color(TEXT));
            });
        });
}

/** @brief 上方小标签、下方控件的组合。 */
fn labeled(ui: &mut Ui, label: &str, add: impl FnOnce(&mut Ui)) {
    ui.vertical(|ui| {
        ui.spacing_mut().item_spacing.y = 2.0;
        ui.label(RichText::new(label).size(11.0).color(MUTED));
        add(ui);
    });
}

/**
 * @brief 表单中的一行数值输入。
 *
 * 键入时不实时提交，回车、失焦或拖动时才算修改，避免循环中半截数字被下发。
 */
fn value(ui: &mut Ui, label: &str, value: &mut f64, unit: &str, speed: f64, range: std::ops::RangeInclusive<f64>) -> bool {
    ui.label(RichText::new(label).color(MUTED));
    let drag = egui::DragValue::new(value).speed(speed).range(range).suffix(unit).max_decimals(4).update_while_editing(false);
    let changed = ui.add_sized([200.0, 28.0], drag).changed();
    ui.end_row();
    changed
}

fn note(ui: &mut Ui, text: &str, color: Color32) {
    ui.add_space(2.0);
    ui.label(RichText::new(text).color(color).size(12.0));
}

fn is_current_channel(channel: Channel) -> bool {
    matches!(channel, Channel::Id | Channel::Iq | Channel::IqRef | Channel::Vq)
}

fn log_color(line: &str) -> Color32 {
    if ["错误", "失败", "拒绝", "ERROR", "INVALID", "FAULT"].iter().any(|key| line.contains(key)) {
        RED_HI
    } else if ["超时", "重试", "警告", "自动切换"].iter().any(|key| line.contains(key)) {
        YELLOW
    } else {
        Color32::from_rgb(0xB4, 0xB4, 0xB0)
    }
}

/** @brief 光标处各曲线的值。点按横轴有序，用二分查找最近点。 */
fn cursor_text(pointer: egui_plot::PlotPoint, scope: &ScopeSnap) -> String {
    let mut parts = vec![format!("x {:.3}", pointer.x)];
    for series in &scope.series {
        let points = &series.points;
        if points.is_empty() {
            continue;
        }
        let at = points.partition_point(|p| p.x < pointer.x);
        let nearest = [at.saturating_sub(1), at.min(points.len() - 1)]
            .into_iter()
            .min_by(|a, b| {
                (points[*a].x - pointer.x).abs().partial_cmp(&(points[*b].x - pointer.x).abs()).unwrap_or(std::cmp::Ordering::Equal)
            })
            .unwrap_or(0);
        parts.push(format!("{} {:.4}", series.name, points[nearest].y));
    }
    parts.join("   ")
}

fn selected(snap: &Snapshot) -> Option<&DeviceSnap> {
    snap.devices.iter().find(|dev| Some(dev.id) == snap.selected)
}

fn opt(value: Option<f64>, decimals: usize) -> String {
    value.map(|v| format!("{v:+.decimals$}")).unwrap_or_else(|| "—".into())
}

fn fmt_num(value: f64) -> String {
    let text = format!("{value:.6}");
    let trimmed = text.trim_end_matches('0').trim_end_matches('.');
    if trimmed.is_empty() { "0".into() } else { trimmed.into() }
}

fn baud_label(baud: u32) -> String {
    if baud >= 1_000_000 {
        format!("{} M", baud / 1_000_000)
    } else {
        format!("{} k", baud / 1000)
    }
}

fn iface_label(name: &str) -> String {
    if is_implemented(name) {
        name.into()
    } else {
        format!("{name}（未实现）")
    }
}
