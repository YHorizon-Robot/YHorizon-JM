//! 后台收发线程。界面只拿快照和下命令，避免阻塞绘制。

use std::sync::mpsc::{self, Receiver, Sender};
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use yfoc_can::{open, CanBus};

use crate::commission::{self, Action, ResultView, Step};
use yfoc_motor_sdk::multi_axis::{self, Event, Plan, Sample};
use crate::session::{BusSession, Command, Snapshot};

enum UiEvent {
    Connect { interface: String, channel: String, bitrate: u32 },
    Disconnect,
    Cmd(Command),
    Commission(Action),
    Group(Plan),
    AckError,
    Shutdown,
}

#[derive(Clone, Default)]
pub struct CommissionView {
    pub progress: String,
    pub node: Option<u8>,
    pub result: Option<Result<ResultView, String>>,
}

#[derive(Default)]
struct CommissionLink {
    busy: AtomicBool,
    cancel: AtomicBool,
    view: Mutex<CommissionView>,
    group: Mutex<GroupView>,
}

#[derive(Default, Clone)]
pub struct GroupView {
    pub message: String,
    pub latest: Vec<Sample>,
    pub history: VecDeque<Vec<Sample>>,
}

/// 界面持有的总线手柄。
pub struct HostLink {
    tx: Sender<UiEvent>,
    // 从连接请求入队到适配器关闭。UI 快照可能比实际连接状态晚一帧。
    can_in_use: Arc<AtomicBool>,
    bitrate: AtomicU32,
    commission: Arc<CommissionLink>,
    snap: Arc<Mutex<Arc<Snapshot>>>,
    join: Option<JoinHandle<()>>,
}

impl HostLink {
    /// 启动后台线程。
    pub fn spawn() -> Self {
        let (tx, rx) = mpsc::channel();
        let snap = Arc::new(Mutex::new(Arc::new(Snapshot::default())));
        let snap_worker = Arc::clone(&snap);
        let can_in_use = Arc::new(AtomicBool::new(false));
        let worker_in_use = Arc::clone(&can_in_use);
        let commission = Arc::new(CommissionLink::default());
        let bench = Arc::clone(&commission);
        let join = thread::spawn(move || worker(rx, snap_worker, &worker_in_use, &bench));
        Self { tx, snap, can_in_use, bitrate: AtomicU32::new(0), commission, join: Some(join) }
    }

    /// 打开适配器。
    pub fn connect(&self, interface: String, channel: String, bitrate: u32) {
        if self.can_in_use.swap(true, Ordering::AcqRel) { return; }
        self.bitrate.store(bitrate, Ordering::Release);
        if self.tx.send(UiEvent::Connect { interface, channel, bitrate }).is_err() {
            self.can_in_use.store(false, Ordering::Release);
        }
    }

    pub fn can_in_use(&self) -> bool {
        self.can_in_use.load(Ordering::Acquire)
    }

    /// 断开。直接电流节点会先收到 0 A。
    pub fn disconnect(&self) {
        self.cancel_commission();
        let _ = self.tx.send(UiEvent::Disconnect);
    }

    /// 交给会话的命令。
    pub fn command(&self, command: Command) {
        if self.commission_busy() {
            if matches!(command, Command::StopAll) { self.cancel_commission(); }
            else { return; }
        }
        let _ = self.tx.send(UiEvent::Cmd(command));
    }

    pub fn commission_busy(&self) -> bool {
        self.commission.busy.load(Ordering::Acquire)
    }

    pub fn commission_view(&self) -> CommissionView {
        self.commission.view.lock().expect("commission view").clone()
    }

    pub fn start_commission(&self, action: Action) {
        if self.commission.busy.swap(true, Ordering::AcqRel) { return; }
        self.commission.cancel.store(false, Ordering::Release);
        *self.commission.view.lock().expect("commission view") = CommissionView {
            progress: "正在连接试验接口".into(), node: Some(action.node()), result: None,
        };
        if self.tx.send(UiEvent::Commission(action)).is_err() {
            self.commission.busy.store(false, Ordering::Release);
        }
    }

    pub fn group_view(&self) -> GroupView {
        self.commission.group.lock().expect("group view").clone()
    }

    pub fn start_group(&self, plan: Plan) {
        if self.commission.busy.swap(true, Ordering::AcqRel) { return; }
        let mut view = self.commission.group.lock().expect("group view");
        *view = GroupView::default();
        if self.bitrate.load(Ordering::Acquire) != 1_000_000 {
            view.message = "请按 1 Mbit/s 重新连接 CAN".into();
            self.commission.busy.store(false, Ordering::Release);
            return;
        }
        self.commission.cancel.store(false, Ordering::Release);
        view.message = "正在检查节点与轨迹限值".into();
        if self.tx.send(UiEvent::Group(plan)).is_err() {
            view.message = "CAN 工作线程已退出".into();
            self.commission.busy.store(false, Ordering::Release);
        }
    }

    pub fn cancel_commission(&self) {
        self.commission.cancel.store(true, Ordering::Release);
    }

    /// 关闭错误弹窗。
    pub fn ack_error(&self) {
        let _ = self.tx.send(UiEvent::AckError);
    }

    /// 取最新快照，只增加引用计数。
    pub fn snapshot(&self) -> Arc<Snapshot> {
        Arc::clone(&self.snap.lock().expect("snapshot"))
    }

    /// 通知线程退出并等待。
    pub fn shutdown(&mut self) {
        self.cancel_commission();
        let _ = self.tx.send(UiEvent::Shutdown);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

impl Drop for HostLink {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn worker(rx: Receiver<UiEvent>, snap: Arc<Mutex<Arc<Snapshot>>>, can_in_use: &AtomicBool, bench: &CommissionLink) {
    let mut session = BusSession::new();
    let mut bus: Option<Box<dyn CanBus>> = None;
    let origin = Instant::now();
    let mut last_pub = Instant::now();
    loop {
        let now_us = || origin.elapsed().as_micros() as u64;
        let wait = if bus.is_some() {
            Duration::from_micros(session.next_wait_us(now_us()).clamp(200, 4_000))
        } else {
            Duration::from_millis(40)
        };
        match rx.recv_timeout(wait) {
            Ok(event) => {
                if !handle(&mut session, &mut bus, event, now_us(), can_in_use, bench) {
                    publish(&session, &snap, now_us());
                    break;
                }
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
        while let Ok(event) = rx.try_recv() {
            if !handle(&mut session, &mut bus, event, now_us(), can_in_use, bench) {
                publish(&session, &snap, now_us());
                return;
            }
        }
        if let Some(bus) = bus.as_mut() {
            session.poll(now_us(), bus);
        }
        if last_pub.elapsed() >= Duration::from_millis(16) {
            publish(&session, &snap, now_us());
            last_pub = Instant::now();
        }
    }
}

fn handle(session: &mut BusSession, bus: &mut Option<Box<dyn CanBus>>, event: UiEvent, now_us: u64, can_in_use: &AtomicBool, bench: &CommissionLink) -> bool {
    match event {
        UiEvent::Shutdown => {
            if let Some(bus) = bus.as_mut() {
                session.disconnect(bus, now_us);
                let _ = bus.shutdown();
            }
            false
        }
        UiEvent::Connect { interface, channel, bitrate } => {
            if let Some(existing) = bus.as_mut() {
                session.disconnect(existing, now_us);
                let _ = existing.shutdown();
            }
            *bus = None;
            match open(&interface, &channel, bitrate) {
                Ok(opened) => {
                    *bus = Some(opened);
                    session.connect_reset(format!("{interface}:{channel}  {bitrate}"));
                }
                Err(err) => {
                    session.set_error(err.message);
                    can_in_use.store(false, Ordering::Release);
                }
            }
            true
        }
        UiEvent::Disconnect => {
            *bench.view.lock().expect("commission view") = CommissionView::default();
            *bench.group.lock().expect("group view") = GroupView::default();
            if let Some(mut existing) = bus.take() {
                session.disconnect(existing.as_mut(), now_us);
                let _ = existing.shutdown();
                drop(existing);
                can_in_use.store(false, Ordering::Release);
            }
            true
        }
        UiEvent::AckError => {
            session.ack_error();
            true
        }
        UiEvent::Group(plan) => {
            let result = if let Some(bus) = bus.as_mut() {
                // Quiesce the manual session and its queues before exclusive bus ownership.
                plan.validate().and_then(|_| session.command(Command::StopAll, now_us, bus))
                    .and_then(|_| multi_axis::execute(bus.as_mut(), &plan, &bench.cancel, |evt| {
                        let mut view = bench.group.lock().expect("group view");
                        match evt {
                            Event::Status(s) => view.message = s,
                            Event::Cycle(samples) => {
                                if samples[0].cycle % 10 == 0 {
                                    if view.history.len() >= 240 { view.history.pop_front(); }
                                    view.history.push_back(samples.clone());
                                }
                                view.latest = samples;
                            }
                        }
                    }))
            } else { Err("请先连接 CAN".into()) };
            bench.group.lock().expect("group view").message = result.unwrap_or_else(|e|format!("已停止 · {e}"));
            bench.busy.store(false, Ordering::Release);
            true
        }
        UiEvent::Commission(action) => {
            let result = if let Some(bus) = bus.as_mut() {
                let read_only = matches!(action, Action::Run { step: Step::Inspect, .. } | Action::Export { .. });
                let stopped = if read_only { Ok(()) } else { session.command(Command::StopAll, now_us, bus) };
                stopped.and_then(|_| commission::execute(bus.as_mut(), action, &bench.cancel, |message| {
                    bench.view.lock().expect("commission view").progress = message;
                }))
            } else { Err("请先连接 CAN".into()) };
            bench.view.lock().expect("commission view").result = Some(result);
            bench.busy.store(false, Ordering::Release);
            true
        }
        UiEvent::Cmd(command) => {
            if command.is_view() {
                session.view_command(command, now_us);
                return true;
            }
            if !session.is_connected() {
                session.set_error("请先连接 CAN");
                return true;
            }
            let Some(bus) = bus.as_mut() else {
                session.set_error("请先连接 CAN");
                return true;
            };
            if let Err(err) = session.command(command, now_us, bus) {
                session.set_error(err);
            }
            true
        }
    }
}

fn publish(session: &BusSession, snap: &Mutex<Arc<Snapshot>>, now_us: u64) {
    let fresh = Arc::new(session.snapshot(now_us));
    *snap.lock().expect("snapshot") = fresh;
}
