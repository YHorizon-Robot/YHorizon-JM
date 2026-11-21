use eframe::egui::{self, RichText};

use crate::commission::{Action, Step};
use crate::session::Snapshot;
use crate::theme::{self, GREEN, MUTED, RED_HI, TEXT, YELLOW};

use super::HostApp;

pub(super) struct Pane {
    node: u8,
    armed: bool,
}
impl Default for Pane {
    fn default() -> Self {
        Self {
            node: 1,
            armed: false,
        }
    }
}

impl Pane {
    pub(super) fn select_node(&mut self, node: u8) {
        self.node = node;
        self.armed = false;
    }
}

impl HostApp {
    pub(super) fn commission_screen(&mut self, ui: &mut egui::Ui, snap: &Snapshot) {
        if !snap.connected {
            self.commission.armed = false;
        }
        let view = self.link.commission_view();
        let busy = self.link.commission_busy();
        let node = self.commission.node;
        let active_result = view.result.as_ref().filter(|_| view.node == Some(node));
        let result = active_result.and_then(|r| r.as_ref().ok());

        ui.label(
            RichText::new("校准与辨识")
                .font(theme::bold(22.0))
                .color(TEXT),
        );
        ui.add_space(8.0);
        ui.add_enabled_ui(!busy, |ui| {
            self.connection_controls(ui, snap);
            ui.horizontal(|ui| {
                ui.label("节点");
                let changed = ui
                    .add(egui::DragValue::new(&mut self.commission.node).range(1..=63))
                    .changed();
                if changed {
                    self.commission.armed = false;
                }
                if theme::primary(ui, snap.connected, "读取节点").clicked() {
                    self.link.start_commission(Action::Run {
                        node: self.commission.node,
                        step: Step::Inspect,
                    });
                }
                if theme::chip(ui, false, "导出参数", None).clicked() && snap.connected {
                    if let Some(path) = rfd::FileDialog::new()
                        .add_filter("JSON", &["json"])
                        .set_file_name(format!("yfoc-motor-{}.json", self.commission.node))
                        .save_file()
                    {
                        self.link.start_commission(Action::Export {
                            node: self.commission.node,
                            path,
                        });
                    }
                }
            });
        });

        ui.add_space(12.0);
        ui.add_enabled(
            !busy && snap.connected,
            egui::Checkbox::new(
                &mut self.commission.armed,
                "确认电机已固定、空载，具备自由行程，并已移除会干涉试验的负载",
            ),
        );
        ui.label(
            RichText::new(
                "试验会暂停当前总线上的运动指令。齿槽试验需正反各预留约四圈电机轴行程。按 Esc 可停止。",
            )
            .color(MUTED),
        );
        ui.add_space(10.0);

        let ready = snap.connected && !busy && self.commission.armed;
        let flags = result.map(|r| r.flags);
        for (step, flag, caption) in [
            (
                Step::Encoder,
                8,
                "拟合电角度零偏、极对数和方向，双编码器板同时计算游标零偏",
            ),
            (Step::Rl, 1, "采集静止电流响应，拟合相电阻和 d/q 轴电感"),
            (Step::Pid, 4, "验证电流 PI、速度 PI 和位置环候选参数"),
            (
                Step::Cog,
                32,
                "正反低速扫描，生成补偿表并比较启用前后的速度纹波",
            ),
        ] {
            ui.horizontal(|ui| {
                if theme::primary(ui, ready, step.label()).clicked() {
                    self.link.start_commission(Action::Run {
                        node: self.commission.node,
                        step,
                    });
                }
                if flags.is_some_and(|f| f & flag != 0) {
                    ui.label(RichText::new("有效").color(GREEN));
                }
                ui.label(RichText::new(caption).color(MUTED));
            });
            ui.add_space(5.0);
        }

        ui.add_space(8.0);
        if busy {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label(&view.progress);
                if theme::chip(ui, false, "停止", Some(RED_HI)).clicked() {
                    self.link.cancel_commission();
                }
            });
        } else if let Some(result) = active_result {
            match result {
                Ok(result) => {
                    ui.label(RichText::new(&result.summary).color(GREEN));
                    if let Some(candidate) = &result.candidate {
                        ui.label(RichText::new("已完成试验，候选参数等待保存。").color(YELLOW));
                        if theme::primary(ui, snap.connected, "保存并回读").clicked() {
                            self.link.start_commission(Action::Save(candidate.clone()));
                        }
                    }
                }
                Err(error) => {
                    ui.label(RichText::new(error).color(RED_HI));
                }
            }
        } else {
            ui.label(RichText::new("连接 CAN 后读取目标节点，再按顺序运行试验。").color(MUTED));
        }
        if flags.is_some_and(|f| f & 32 != 0)
            && theme::primary(ui, !busy && snap.connected, "关闭齿槽补偿并保存").clicked()
        {
            self.link.start_commission(Action::DisableCog {
                node: self.commission.node,
            });
        }
    }
}
