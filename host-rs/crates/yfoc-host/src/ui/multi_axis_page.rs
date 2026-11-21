use super::*;
use yfoc_motor_sdk::multi_axis::{Axis, Plan, MAX_AXES};

#[derive(Default)]
struct Row {
    node: u8,
    first: String,
    second: String,
}

pub(super) struct Pane {
    rows: Vec<Row>,
    two: bool,
    seconds: f64,
    plot_node: u8,
}

impl Default for Pane {
    fn default() -> Self {
        Self {
            rows: vec![Row {
                node: 1,
                ..Row::default()
            }],
            two: false,
            seconds: 1.0,
            plot_node: 1,
        }
    }
}

impl Pane {
    fn plan(&self) -> Result<Plan, String> {
        let mut axes = Vec::new();
        for row in &self.rows {
            let parse = |text: &str, segment| text.trim().parse::<f64>()
                .map_err(|_| format!("请填写节点 {} 的第{segment}段目标", row.node));
            axes.push(Axis {
                node: row.node,
                target: parse(&row.first, 1)?,
                second: self.two.then(|| parse(&row.second, 2)).transpose()?,
            });
        }
        let plan = Plan { axes, seconds: self.seconds };
        plan.validate()?;
        Ok(plan)
    }
}

impl HostApp {
    pub(super) fn multi_axis_screen(&mut self, ui: &mut Ui, snap: &Snapshot) {
        let busy = self.link.commission_busy();
        let view = self.link.group_view();
        theme::card().show(ui, |ui| {
            theme::section(ui, "同步位置轨迹");
            ui.label("同步 200 Hz  ·  每轴反馈 200 Hz  ·  经典 CAN 1 Mbit/s");
            ui.add_enabled_ui(!busy, |ui| {
                ui.horizontal_wrapped(|ui| self.connection_controls(ui, snap));
                ui.horizontal_wrapped(|ui| {
                    ui.label("每段时长");
                    ui.add(egui::DragValue::new(&mut self.group.seconds).speed(0.05).range(0.2..=60.0).suffix(" s"));
                    ui.checkbox(&mut self.group.two, "连续执行第二段");
                    if ui.add_enabled(snap.connected, egui::Button::new("扫描节点")).clicked() {
                        self.link.command(Command::Scan);
                    }
                });
                let mut remove = None;
                let can_remove = self.group.rows.len() > 1;
                egui::Grid::new("sync_targets").striped(true).show(ui, |ui| {
                    ui.label("节点"); ui.label("当前位置 (rad)"); ui.label("第一目标 (rad)");
                    if self.group.two { ui.label("第二目标 (rad)"); }
                    ui.end_row();
                    for (i,row) in self.group.rows.iter_mut().enumerate() {
                        if ui.add(egui::DragValue::new(&mut row.node).range(1..=63)).changed() {
                            row.first.clear();
                            row.second.clear();
                        }
                        let pos = if busy {
                            view.latest.iter().find(|s| s.node == row.node).map(|s| s.position)
                        } else {
                            snap.devices.iter().find(|x| x.id == row.node && x.online && !x.stale).and_then(|x|x.pos)
                        };
                        ui.label(pos.map_or_else(|| "—".into(), |p| format!("{p:.3}")));
                        ui.add(egui::TextEdit::singleline(&mut row.first).desired_width(95.0));
                        if self.group.two { ui.add(egui::TextEdit::singleline(&mut row.second).desired_width(95.0)); }
                        if ui.add_enabled(pos.is_some(), egui::Button::new("用当前位置填入")).clicked() {
                            if let Some(p) = pos { row.first = format!("{p:.3}"); row.second.clone_from(&row.first); }
                        }
                        if ui.add_enabled(can_remove, egui::Button::new("移除")).clicked() { remove = Some(i); }
                        ui.end_row();
                    }
                });
                if let Some(i) = remove { self.group.rows.remove(i); }
                if ui.add_enabled(self.group.rows.len() < MAX_AXES, egui::Button::new("添加轴")).clicked() {
                    let candidates = snap.devices.iter().filter(|d|d.online && !d.stale).map(|d|d.id).chain(1..=63);
                    if let Some(node) = candidates.into_iter().find(|n| self.group.rows.iter().all(|r|r.node != *n)) {
                        self.group.rows.push(Row{node,..Row::default()});
                    }
                }
                ui.label("目标为输出轴绝对位置。开始时先停止总线上的手动驱动，全部节点就绪后共同起动，到位后关桥。");
                let plan = self.group.plan();
                if let Err(e) = &plan { ui.label(e); }
                if theme::primary(ui, snap.connected && plan.is_ok(), "开始同步运动").clicked() {
                    if let Ok(plan) = plan {
                        self.group.plot_node = plan.axes[0].node;
                        self.link.start_group(plan);
                    }
                }
            });
        });
        ui.add_space(10.0);
        theme::card().show(ui, |ui| {
            theme::section(ui, "同步反馈");
            ui.label(if view.message.is_empty() {
                "等待启动"
            } else {
                &view.message
            });
            if view.latest.is_empty() {
                return;
            }
            ui.label(format!("周期 {}", view.latest[0].cycle));
            egui::Grid::new("sync_measurements")
                .striped(true)
                .show(ui, |ui| {
                    for h in [
                        "轴 / 曲线",
                        "参考 (rad)",
                        "实测 (rad)",
                        "误差 (rad)",
                        "速度 (rad/s)",
                    ] {
                        ui.label(h);
                    }
                    ui.end_row();
                    for sample in &view.latest {
                        ui.selectable_value(
                            &mut self.group.plot_node,
                            sample.node,
                            format!("节点 {}", sample.node),
                        );
                        for x in [
                            sample.reference,
                            sample.position,
                            sample.reference - sample.position,
                            sample.velocity,
                        ] {
                            ui.monospace(format!("{x:.3}"));
                        }
                        ui.end_row();
                    }
                });
            Plot::new("sync_position")
                .height(190.0)
                .legend(Legend::default())
                .x_axis_label("时间 (s)")
                .y_axis_label("输出轴 (rad)")
                .show(ui, |plot| {
                    for (label, actual) in [("参考", false), ("实测", true)] {
                        let points: Vec<[f64; 2]> = view
                            .history
                            .iter()
                            .filter_map(|r| r.iter().find(|s| s.node == self.group.plot_node))
                            .map(|s| {
                                [
                                    f64::from(s.cycle) * 0.005,
                                    if actual { s.position } else { s.reference },
                                ]
                            })
                            .collect();
                        plot.line(Line::new(label, points));
                    }
                });
        });
    }
}
