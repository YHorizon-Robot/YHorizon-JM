//! 烧录页。和电机调试分开，只负责选板、选探针、写固件。

use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};

use eframe::egui::{self, Align, Color32, CornerRadius, Frame, Layout, Margin, RichText, ScrollArea, Vec2};

use yfoc_flash::{flash_image, list_probes, Board, FlashJob, ProbeId};

use super::{note, HostApp};
use crate::theme::{self, GREEN, MUTED, RED_HI, YELLOW};

/** @brief 烧录页上需要跨帧记住的选择。 */
pub struct FlashPane {
    link: FlashLink,
    board: usize,
    probe: Option<ProbeId>,
    path: String,
    verify: bool,
    reset_run: bool,
    overwrite_nv: bool,
    armed: bool,
    pending: bool,
    seen_scan: u64,
    seen_finished: u64,
}

impl FlashPane {
    pub fn new() -> Self {
        Self {
            link: FlashLink::spawn(),
            board: 0,
            probe: None,
            path: String::new(),
            verify: true,
            reset_run: true,
            overwrite_nv: false,
            armed: false,
            pending: false,
            seen_scan: 0,
            seen_finished: 0,
        }
    }

    pub(super) fn active(&self) -> bool {
        let snap = self.link.snapshot();
        self.pending || snap.running || snap.scanning
    }

    pub(super) fn programming(&self) -> bool {
        self.pending
    }
}

impl HostApp {
    /** @brief 烧录页正文。进入时扫描一次探针。 */
    pub(super) fn flash_screen(&mut self, ui: &mut egui::Ui, can_connected: bool) {
        if !self.flash.armed {
            self.flash.armed = true;
            self.flash.link.scan();
        }
        let snap = self.flash.link.snapshot();
        if snap.scan_id != self.flash.seen_scan {
            self.flash.seen_scan = snap.scan_id;
            let kept = self.flash.probe.as_ref().and_then(|current| snap.probes.iter().find(|item| *item == current).cloned());
            self.flash.probe = kept.or_else(|| snap.probes.first().cloned());
        }
        if self.flash.pending && snap.finished != self.flash.seen_finished {
            self.flash.pending = false;
            self.flash.seen_finished = snap.finished;
        }

        let gap = 12.0;
        let avail = ui.available_width();
        if avail < 980.0 {
            self.flash_setup(ui, &snap, can_connected);
            ui.add_space(gap);
            flash_log(ui, &snap);
            return;
        }
        let left = ((avail - gap) * 0.46).floor();
        let right = avail - gap - left;
        ui.horizontal_top(|ui| {
            ui.spacing_mut().item_spacing.x = gap;
            ui.allocate_ui_with_layout(Vec2::new(left, 0.0), Layout::top_down(Align::Min), |ui| {
                ui.set_width(left);
                self.flash_setup(ui, &snap, can_connected);
            });
            ui.allocate_ui_with_layout(Vec2::new(right, 0.0), Layout::top_down(Align::Min), |ui| {
                ui.set_width(right);
                flash_log(ui, &snap);
            });
        });
    }

    fn flash_setup(&mut self, ui: &mut egui::Ui, snap: &FlashSnap, can_connected: bool) {
        let busy = self.flash.pending || snap.running;
        theme::card().show(ui, |ui| {
            ui.set_width(ui.available_width());
            if busy {
                ui.disable();
            }
            theme::section(ui, "烧录固件");
            ui.add_space(4.0);
            note(ui, "通过 SWD 把 ELF、HEX 或 BIN 写进芯片。探针只要驱动能识别、并且能写 Flash，都会列在这里。", MUTED);
            ui.add_space(10.0);

            if can_connected {
                note(ui, "CAN 还连着。烧录会复位电机，请先断开。", YELLOW);
                ui.add_space(6.0);
                if ui.button("断开 CAN").clicked() {
                    self.link.disconnect();
                }
                ui.add_space(8.0);
            }

            super::labeled(ui, "板卡", |ui| {
                let board = Board::ALL[self.flash.board];
                egui::ComboBox::from_id_salt("flash-board")
                    .selected_text(format!("{} · {}", board.label(), board.chip_label()))
                    .width(ui.available_width())
                    .show_ui(ui, |ui| {
                        for (index, board) in Board::ALL.iter().enumerate() {
                            ui.selectable_value(
                                &mut self.flash.board,
                                index,
                                format!("{} · {}", board.label(), board.chip_label()),
                            );
                        }
                    });
            });
            ui.add_space(8.0);
            super::labeled(ui, "烧录器", |ui| {
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 4.0;
                    let selected = self.flash.probe.as_ref().map(|probe| probe.label.clone()).unwrap_or_else(|| {
                        if snap.scanning { "正在扫描…".into() } else { "未发现烧录器".into() }
                    });
                    egui::ComboBox::from_id_salt("flash-probe")
                        .selected_text(selected)
                        .width((ui.available_width() - 36.0).max(160.0))
                        .show_ui(ui, |ui| {
                            for probe in &snap.probes {
                                let chosen = self.flash.probe.as_ref() == Some(probe);
                                if ui.selectable_label(chosen, &probe.label).clicked() {
                                    self.flash.probe = Some(probe.clone());
                                }
                            }
                            if snap.probes.is_empty() {
                                ui.label(RichText::new("没有探针").color(MUTED));
                            }
                        });
                    if ui.add_enabled(!busy, egui::Button::new("⟳")).on_hover_text("重新扫描烧录器").clicked() {
                        self.flash.link.scan();
                    }
                });
            });
            ui.add_space(8.0);
            super::labeled(ui, "固件", |ui| {
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 4.0;
                    let edit = egui::TextEdit::singleline(&mut self.flash.path).desired_width(ui.available_width() - 64.0).hint_text("ELF / HEX / BIN");
                    ui.add_enabled(!busy, edit);
                    if ui.add_enabled(!busy, egui::Button::new("浏览")).clicked() {
                        if let Some(path) = rfd::FileDialog::new()
                            .add_filter("固件", &["elf", "hex", "bin"])
                            .pick_file()
                        {
                            self.flash.path = path.display().to_string();
                        }
                    }
                });
            });
            ui.add_space(10.0);
            ui.add_enabled(!busy, egui::Checkbox::new(&mut self.flash.verify, "写入后校验"));
            ui.add_enabled(!busy, egui::Checkbox::new(&mut self.flash.reset_run, "烧完复位运行"));
            ui.add_enabled(!busy, egui::Checkbox::new(&mut self.flash.overwrite_nv, "允许覆盖参数区"));
            if self.flash.overwrite_nv {
                note(ui, "固件若与参数区重叠，可能覆盖已保存的标定或电机参数。", YELLOW);
            } else {
                let board = Board::ALL[self.flash.board];
                note(
                    ui,
                    &format!("默认保留 0x{:08X} 起的电机参数和标定页。", board.parameter_addr()),
                    MUTED,
                );
            }
            ui.add_space(10.0);
            let ready = !busy && !can_connected && self.flash.probe.is_some() && !self.flash.path.trim().is_empty();
            let label = if busy { "烧录中" } else { "开始烧录" };
            if theme::primary(ui, ready, label).clicked() {
                if let Some(probe) = self.flash.probe.clone() {
                    self.flash.pending = true;
                    self.flash.link.start(FlashJob {
                        probe,
                        board: Board::ALL[self.flash.board],
                        image: PathBuf::from(self.flash.path.trim()),
                        verify: self.flash.verify,
                        reset_run: self.flash.reset_run,
                        overwrite_nv: self.flash.overwrite_nv,
                    });
                }
            }
            ui.add_space(4.0);
            note(ui, "开始之后会一直写完。中途拔掉探针可能留下半片固件。", MUTED);
            if snap.probes.is_empty() && !snap.scanning {
                note(ui, "没扫到探针时，确认已经插上。ST-Link 需要系统里已经装过的 USB 驱动。", MUTED);
            }
            if let Some(banner) = &snap.banner {
                ui.add_space(6.0);
                note(ui, banner, if snap.banner_ok { GREEN } else { RED_HI });
            }
        });
    }
}

fn flash_log(ui: &mut egui::Ui, snap: &FlashSnap) {
    theme::card().show(ui, |ui| {
        ui.set_width(ui.available_width());
        ui.horizontal(|ui| {
            theme::section(ui, "烧录进度");
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let text = if snap.running {
                    "进行中"
                } else if snap.scanning {
                    "扫描中"
                } else if snap.banner_ok {
                    "完成"
                } else if snap.banner.is_some() {
                    "失败"
                } else {
                    "空闲"
                };
                let color = if snap.running || snap.scanning {
                    YELLOW
                } else if snap.banner_ok {
                    GREEN
                } else if snap.banner.is_some() {
                    RED_HI
                } else {
                    MUTED
                };
                theme::badge(ui, text, color);
            });
        });
        ui.add_space(8.0);
        let frac = if snap.total > 0 {
            (snap.done as f32 / snap.total as f32).clamp(0.0, 1.0)
        } else {
            0.0
        };
        let phase = if snap.phase.is_empty() { "就绪" } else { snap.phase.as_str() };
        ui.add(egui::ProgressBar::new(frac).show_percentage().text(phase).desired_width(ui.available_width()));
        ui.add_space(8.0);
        Frame::new()
            .fill(Color32::from_rgb(0x0B, 0x0B, 0x0D))
            .corner_radius(CornerRadius::same(8))
            .inner_margin(Margin::same(8))
            .show(ui, |ui| {
                ScrollArea::vertical().id_salt("flash-log").auto_shrink([false, false]).max_height(420.0).stick_to_bottom(true).show(ui, |ui| {
                    if snap.log.is_empty() {
                        ui.label(RichText::new("烧录日志会显示在这里。").color(MUTED));
                    }
                    for line in &snap.log {
                        ui.label(RichText::new(line).monospace().size(12.0).color(super::log_color(line)));
                    }
                });
            });
    });
}

struct FlashSnap {
    probes: Vec<ProbeId>,
    scanning: bool,
    running: bool,
    phase: String,
    done: u64,
    total: u64,
    log: Vec<String>,
    banner: Option<String>,
    banner_ok: bool,
    scan_id: u64,
    finished: u64,
}

struct Shared {
    probes: Vec<ProbeId>,
    scanning: bool,
    running: bool,
    phase: String,
    done: u64,
    total: u64,
    log: Vec<String>,
    banner: Option<String>,
    banner_ok: bool,
    scan_id: u64,
    finished: u64,
}

impl Shared {
    fn snap(&self) -> FlashSnap {
        FlashSnap {
            probes: self.probes.clone(),
            scanning: self.scanning,
            running: self.running,
            phase: self.phase.clone(),
            done: self.done,
            total: self.total,
            log: self.log.clone(),
            banner: self.banner.clone(),
            banner_ok: self.banner_ok,
            scan_id: self.scan_id,
            finished: self.finished,
        }
    }
}

enum Msg {
    Scan,
    Flash(FlashJob),
    Shutdown,
}

struct FlashLink {
    tx: Sender<Msg>,
    shared: Arc<Mutex<Shared>>,
    join: Option<JoinHandle<()>>,
}

impl FlashLink {
    fn spawn() -> Self {
        let (tx, rx) = mpsc::channel();
        let shared = Arc::new(Mutex::new(Shared {
            probes: Vec::new(),
            scanning: false,
            running: false,
            phase: String::new(),
            done: 0,
            total: 0,
            log: Vec::new(),
            banner: None,
            banner_ok: false,
            scan_id: 0,
            finished: 0,
        }));
        let worker_state = Arc::clone(&shared);
        let join = thread::Builder::new()
            .name("yfoc-flash".into())
            .spawn(move || flash_worker(rx, worker_state))
            .expect("flash thread");
        Self { tx, shared, join: Some(join) }
    }

    fn scan(&self) {
        let _ = self.tx.send(Msg::Scan);
    }

    fn start(&self, job: FlashJob) {
        let _ = self.tx.send(Msg::Flash(job));
    }

    fn snapshot(&self) -> FlashSnap {
        self.shared.lock().expect("flash state").snap()
    }
}

impl Drop for FlashLink {
    fn drop(&mut self) {
        let _ = self.tx.send(Msg::Shutdown);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

fn flash_worker(rx: Receiver<Msg>, shared: Arc<Mutex<Shared>>) {
    while let Ok(msg) = rx.recv() {
        match msg {
            Msg::Shutdown => break,
            Msg::Scan => {
                {
                    let mut state = shared.lock().expect("flash state");
                    state.scanning = true;
                }
                let probes = list_probes();
                let count = probes.len();
                let mut state = shared.lock().expect("flash state");
                state.probes = probes;
                state.scanning = false;
                state.scan_id = state.scan_id.wrapping_add(1);
                push_log(&mut state.log, format!("扫描到 {count} 只烧录器"));
            }
            Msg::Flash(job) => {
                {
                    let mut state = shared.lock().expect("flash state");
                    state.running = true;
                    state.banner = None;
                    state.banner_ok = false;
                    state.phase = "准备".into();
                    state.done = 0;
                    state.total = 0;
                    state.log.clear();
                    push_log(&mut state.log, format!("烧录 {} → {}", job.image.display(), job.board.label()));
                }
                let state_cb = Arc::clone(&shared);
                let result = flash_image(&job, |progress| apply_progress(&state_cb, progress));
                let mut state = shared.lock().expect("flash state");
                state.running = false;
                state.finished = state.finished.wrapping_add(1);
                match result {
                    Ok(()) => {
                        state.banner_ok = true;
                        state.banner = Some("烧录完成".into());
                        state.phase = "完成".into();
                        if state.total > 0 {
                            state.done = state.total;
                        }
                    }
                    Err(err) => {
                        state.banner_ok = false;
                        state.banner = Some(err.clone());
                        state.phase = "失败".into();
                        push_log(&mut state.log, format!("失败: {err}"));
                    }
                }
            }
        }
    }
}

fn apply_progress(shared: &Mutex<Shared>, progress: yfoc_flash::Progress) {
    let mut state = shared.lock().expect("flash state");
    if !progress.phase.is_empty() {
        state.phase = progress.phase;
    }
    state.done = progress.done;
    if progress.total > 0 {
        state.total = progress.total;
    }
    if let Some(line) = progress.log {
        push_log(&mut state.log, line);
    }
}

fn push_log(log: &mut Vec<String>, line: String) {
    log.push(line);
    if log.len() > 400 {
        let extra = log.len() - 400;
        log.drain(0..extra);
    }
}
