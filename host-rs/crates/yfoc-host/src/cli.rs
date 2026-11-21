//! `yfoc-host` 的命令行。没有查询或 `--run` 时，程序改开图形界面。

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use clap::Parser;
use yfoc_can::{default_backend, default_channel, detect_channels, list_channels, open, CanBus};
use yfoc_protocol::{
    current_command_to_nm, current_scale, pack_motion, unpack_ack, INNER_LOOP_CURRENT,
};

use yfoc_motor_sdk::Joint;
use crate::commission::{self, Action, Step};
use yfoc_motor_sdk::multi_axis::{self, Axis, Event, Plan};
use crate::session::BusSession;

/// YHorizon-JM 伺服与电流环上位机。
#[derive(Parser, Debug)]
#[command(name = "yfoc-host", version, about = "YHorizon-JM servo and current-loop host")]
pub struct Args {
    /// 打开图形界面。
    #[arg(long)]
    pub gui: bool,
    /// 列出通道后退出。
    #[arg(long)]
    pub list_can: bool,
    /// 扫描总线上的节点后退出。
    #[arg(long)]
    pub scan: bool,
    /// 接口名：pcan、socketcan、slcan。
    #[arg(long)]
    pub interface: Option<String>,
    /// 通道，例如 PCAN_USBBUS1 或 can0。
    #[arg(long)]
    pub channel: Option<String>,
    /// 波特率。SocketCAN 由系统接口决定。
    #[arg(long, default_value_t = 1_000_000)]
    pub bitrate: u32,
    /// 电机节点，1~63。
    #[arg(long, default_value_t = 1)]
    pub id: u8,
    /// 目标位置，rad。出现这个参数就会走命令行运动流。
    #[arg(long)]
    pub pos: Option<f64>,
    /// 目标速度，rad/s。
    #[arg(long)]
    pub vel: Option<f64>,
    /// 运控 Kp，输出轴 Nm/rad。
    #[arg(long)]
    pub kp: Option<f64>,
    /// 运控 Kd，输出轴 Nm·s/rad。
    #[arg(long)]
    pub kd: Option<f64>,
    /// 运控前馈，输出轴 Nm。
    #[arg(long)]
    pub ff: Option<f64>,
    /// 切换内环：voltage 或 current。
    #[arg(long)]
    pub inner_loop: Option<String>,
    /// 直接电流，A。不会偷偷切换内环。
    #[arg(long)]
    pub current: Option<f64>,
    /// 读电流实测后退出。
    #[arg(long)]
    pub current_state: bool,
    /// 读电流配置后退出。
    #[arg(long)]
    pub current_config: bool,
    /// 读状态后退出。
    #[arg(long)]
    pub status: bool,
    /// 循环频率，Hz。
    #[arg(long, default_value_t = 200.0)]
    pub rate: f64,
    /// 只监听，不查询。
    #[arg(long)]
    pub listen: bool,
    /// 收到一次反馈后退出。直接电流仍会先发 0 A。
    #[arg(long)]
    pub once: bool,
    /// 即使没有其它运动参数，也按默认 0 发送 Motion 流。
    #[arg(long)]
    pub run: bool,
    /// 校准、辨识、自动整定、补偿或参数导出。
    #[arg(long, value_enum, conflicts_with_all = ["run", "listen", "scan", "pos", "vel", "current", "inner_loop"])]
    pub commission: Option<Step>,
    /// 确认可在限幅台架上试验，输出轴具有所需的自由行程。
    #[arg(long, requires = "commission")]
    pub commission_arm: bool,
    /// 通过试验后保存当前步骤的参数。
    #[arg(long, requires = "commission")]
    pub commission_save: bool,
    /// 参数导出的 JSON 文件路径。
    #[arg(long, requires = "commission")]
    pub commission_output: Option<std::path::PathBuf>,
    /// 同步轴 ID=第一目标[/第二目标]，输出轴绝对 rad，可重复指定。
    #[arg(long, value_parser = parse_axis, conflicts_with_all = ["gui", "list_can", "scan", "listen", "once", "status", "run", "pos", "vel", "kp", "kd", "ff", "current", "current_state", "current_config", "inner_loop", "commission"])]
    pub sync_axis: Vec<Axis>,
    /// 同步轨迹每段时长，秒。
    #[arg(long, default_value_t = 1.0)]
    pub sync_seconds: f64,
    /// 执行同步位置运动。
    #[arg(long, requires = "sync_axis")]
    pub sync_arm: bool,
    /// 导出完整同步周期的反馈 CSV。
    #[arg(long, requires = "sync_axis")]
    pub sync_log: Option<std::path::PathBuf>,
}

impl Args {
    /// 这些参数表示用户要命令行，而不是图形界面。
    pub fn cli_requested(&self) -> bool {
        self.list_can
            || !self.sync_axis.is_empty()
            || self.commission.is_some()
            || self.scan
            || self.listen
            || self.once
            || self.status
            || self.current.is_some()
            || self.current_state
            || self.current_config
            || self.inner_loop.is_some()
            || self.run
            || self.pos.is_some()
            || self.vel.is_some()
            || self.kp.is_some()
            || self.kd.is_some()
            || self.ff.is_some()
    }

    fn interface_name(&self) -> String {
        self.interface.clone().unwrap_or_else(|| default_backend().as_str().into())
    }

    fn channel_name(&self) -> String {
        self.channel.clone().unwrap_or_else(|| default_channel(&self.interface_name()))
    }
}

/// 执行命令行。返回进程退出码。
pub fn run(args: Args) -> i32 {
    if args.list_can {
        let interface = args.interface_name();
        let found = detect_channels(&interface);
        println!("interface={interface} default_channel={}", default_channel(&interface));
        println!("detected: {}", if found.is_empty() { "(none)".into() } else { found.join(", ") });
        println!("choices:  {}", list_channels(&interface).join(", "));
        return 0;
    }
    if args.listen && (args.current.is_some() || args.inner_loop.is_some() || args.current_state || args.current_config || args.status) {
        eprintln!("--listen is passive; do not combine it with commands");
        return 1;
    }
    let bus = match open(&args.interface_name(), &args.channel_name(), args.bitrate) {
        Ok(bus) => bus,
        Err(err) => {
            eprintln!("{err}");
            return 1;
        }
    };
    if args.scan {
        return scan_bus(bus);
    }
    if !args.sync_axis.is_empty() {
        return match run_sync(&args, bus) {
            Ok(()) => 0,
            Err(e) => { eprintln!("{e}"); 1 }
        };
    }
    if let Some(step) = args.commission {
        return match run_commission(&args, step, bus) {
            Ok(()) => 0,
            Err(error) => { eprintln!("{error}"); 1 }
        };
    }
    match run_bus(args, bus) {
        Ok(code) => code,
        Err(err) => {
            eprintln!("{err}");
            1
        }
    }
}

fn parse_axis(text: &str) -> Result<Axis, String> {
    let (node, targets) = text.split_once('=').ok_or("格式为 ID=目标[/第二目标]")?;
    let (first, second) = targets.split_once('/').map_or((targets,None), |(a,b)|(a,Some(b)));
    Ok(Axis {
        node: node.parse().map_err(|_|"节点号无效")?,
        target: first.parse().map_err(|_|"第一目标无效")?,
        second: second.map(str::parse).transpose().map_err(|_|"第二目标无效")?,
    })
}

fn run_sync(args: &Args, mut bus: Box<dyn CanBus>) -> Result<(),String> {
    use std::io::{BufWriter,Write};
    let plan = Plan {axes:args.sync_axis.clone(),seconds:args.sync_seconds};
    plan.validate()?;
    if !args.sync_arm || args.bitrate != 1_000_000 { return Err("同步运动需要 --sync-arm 和 1 Mbit/s CAN".into()); }
    let file = args.sync_log.as_ref().map(std::fs::File::create_new).transpose().map_err(|e|e.to_string())?;
    let cancel = Arc::new(AtomicBool::new(false));
    let signal = cancel.clone();
    ctrlc::set_handler(move || signal.store(true,Ordering::Release)).map_err(|e|e.to_string())?;
    let mut samples = Vec::new();
    let result = multi_axis::execute(bus.as_mut(), &plan, &cancel, |event| match event {
        Event::Status(message) => println!("{message}"),
        Event::Cycle(row) => if file.is_some() {samples.extend(row);},
    });
    let closed = bus.shutdown();
    if let Some(file) = file {
        let mut writer = BufWriter::new(file);
        let saved = (|| -> std::io::Result<()> {
            writeln!(writer,"host_us,cycle,node,reference_rad,position_rad,velocity_rad_s")?;
            for s in samples {
                writeln!(writer,"{},{},{},{:.6},{:.3},{:.3}",s.host_us,s.cycle,s.node,s.reference,s.position,s.velocity)?;
            }
            writer.flush()
        })();
        saved.map_err(|e|e.to_string())?;
    }
    println!("{}",result?);
    closed.map_err(|e|e.to_string())
}

fn run_commission(args: &Args, step: Step, mut bus: Box<dyn CanBus>) -> Result<(), String> {
    if step.moves() && !args.commission_arm {
        return Err("旋转试验须确认台架和行程后指定 --commission-arm".into());
    }
    let action = match step {
        Step::Export => Action::Export {
            node: args.id,
            path: args.commission_output.clone().ok_or("导出需要 --commission-output <文件>")?,
        },
        _ => Action::Run { node: args.id, step },
    };
    let cancel = Arc::new(AtomicBool::new(false));
    let signal = Arc::clone(&cancel);
    ctrlc::set_handler(move || signal.store(true, Ordering::Relaxed)).map_err(|e| e.to_string())?;
    let result = commission::execute(bus.as_mut(), action, &cancel, |message| println!("{message}"));
    let result = result.and_then(|result| {
        println!("{}", result.summary);
        if let Some(candidate) = result.candidate.filter(|_| args.commission_save) {
            let saved = commission::execute(bus.as_mut(), Action::Save(candidate), &cancel, |message| println!("{message}"))?;
            println!("{}", saved.summary);
        }
        Ok(())
    });
    let _ = bus.shutdown();
    result
}

fn scan_bus(mut bus: Box<dyn CanBus>) -> i32 {
    let mut session = BusSession::new();
    session.connect_reset("scan");
    if let Err(err) = session.command(crate::session::Command::Scan, 0, bus.as_mut()) {
        eprintln!("{err}");
        return 1;
    }
    let started = Instant::now();
    while started.elapsed() < Duration::from_millis(900) {
        let now = started.elapsed().as_micros() as u64;
        session.poll(now, bus.as_mut());
        thread::sleep(Duration::from_millis(1));
    }
    let snap = session.snapshot(started.elapsed().as_micros() as u64);
    if snap.devices.is_empty() {
        println!("scan: no nodes answered");
    }
    for dev in snap.devices {
        if dev.online {
            println!(
                "id={} board={} fw={} state={} fault={} mode={} inner={}",
                dev.id, dev.board_name, dev.fw, dev.state_name, dev.fault_name, dev.mode_name, dev.inner_text
            );
        }
    }
    let _ = bus.shutdown();
    0
}

fn run_bus(args: Args, bus: Box<dyn CanBus>) -> Result<i32, String> {
    let mut client = Joint::new(bus, args.id)?;
    let mut current_started = false;
    let mut supports_disable = false;
    let result = (|| {
        if args.listen {
            return listen_loop(&mut client);
        }
        let info = client.discover()?;
        supports_disable = info.as_ref().is_some_and(|device| device.control_ext);
        match info {
            Some(info) => println!(
                "device {} fw={}.{} caps=0x{:02X} ntc={} abs_warn={}",
                info.board_name, info.fw_major, info.fw_minor, info.caps, info.temp_sensor, info.position_warn
            ),
            None => println!("legacy/unknown firmware: pu only"),
        }
        let mut status = client.status()?;
        print_status(&status);
        if let Ok(data) = client.mgmt(yfoc_protocol::CMD_GET_FEEDBACK, 0, Some(client.ids().fb), None) {
            println!("{}", Joint::<Box<dyn CanBus>>::feedback_line(data));
        }
        let config = if client.supports_current() { Some(client.current_config()?) } else { None };
        if let Some(config) = &config {
            println!(
                "current limits ±{}A / ±{}pu; scale={}A/pu; BW={}Hz; Kp={}V/A Ki={}V/(A*s) loop={}Hz",
                fmt_num(config.i_limit.unwrap_or(0.0)),
                fmt_num(config.v_limit.unwrap_or(0.0)),
                fmt_num(current_scale(Some(config)).unwrap_or(0.0)),
                config.bandwidth_hz.unwrap_or(0),
                fmt_num(config.kp.unwrap_or(0.0)),
                fmt_num(config.ki.unwrap_or(0.0)),
                config.loop_hz.unwrap_or(0)
            );
        }
        if let Some(inner) = &args.inner_loop {
            let current = match inner.as_str() {
                "current" => true,
                "voltage" => false,
                other => return Err(format!("unknown --inner-loop {other}")),
            };
            client.set_inner(current)?;
            status = client.status()?;
            print_status(&status);
            let want = if current { INNER_LOOP_CURRENT } else { yfoc_protocol::INNER_LOOP_VOLTAGE };
            if status.inner_loop != Some(want) {
                return Err("Inner-loop switch was not confirmed by status".into());
            }
        }
        if args.current_state {
            let state = client.current_state()?;
            println!(
                "current Id={:+.3}A Iq={:+.3}A Iq_ref={:+.3}A Vq={:+.4}pu",
                state.id, state.iq, state.iq_ref, state.vq
            );
        }
        if args.current_config && !client.supports_current() {
            return Err("Current configuration is unavailable on this firmware".into());
        }
        if args.current.is_none() && (args.status || args.current_state || args.current_config || args.inner_loop.is_some()) {
            return Ok(0);
        }
        if !(args.rate > 0.0 && args.rate <= 2000.0) {
            return Err("--rate must be in (0, 2000] Hz".into());
        }
        let motion = if let Some(amps) = args.current {
            if status.inner_loop != Some(INNER_LOOP_CURRENT) {
                return Err("--current requires confirmed CURRENT mode; use --inner-loop current".into());
            }
            let nm = current_command_to_nm(amps, config.as_ref())?;
            client.motion_setup(0.0, 0.0)?;
            pack_motion(0.0, 0.0, nm)
        } else {
            client.motion_setup(args.kp.unwrap_or(0.0), args.kd.unwrap_or(0.0))?;
            pack_motion(args.pos.unwrap_or(0.0), args.vel.unwrap_or(0.0), args.ff.unwrap_or(0.0))
        };
        let stop = Arc::new(AtomicBool::new(false));
        let flag = Arc::clone(&stop);
        let _ = ctrlc::set_handler(move || flag.store(true, Ordering::Relaxed));
        let period = Duration::from_secs_f64(1.0 / args.rate.max(1.0));
        let mut sent_once = false;
        let mut next_tx = Instant::now();
        let once_deadline = Instant::now() + Duration::from_secs(2);
        let mut got_feedback = false;
        let mut got_current = false;
        let mut next_status = Instant::now() + Duration::from_secs(1);
        loop {
            if stop.load(Ordering::Relaxed) {
                return Ok(0);
            }
            let now = Instant::now();
            let due = !sent_once || (!args.once && now >= next_tx);
            if due {
                client.send(client.ids().motion, motion)?;
                sent_once = true;
                current_started = args.current.is_some();
                next_tx = now + period;
            }
            if args.once && now >= once_deadline {
                return Err(if got_feedback { "Timed out waiting for current state" } else { "Timed out waiting for feedback" }.into());
            }
            if client.supports_current() && now >= next_status {
                client.poke_status()?;
                next_status = now + Duration::from_secs(1);
            }
            if let Some(frame) = client.recv(Duration::from_millis(1))? {
                if frame.id == client.ids().fb {
                    println!("{}", Joint::<Box<dyn CanBus>>::feedback_line(frame.data));
                    if args.once && current_started && !got_feedback {
                        client.poke_current()?;
                    }
                    got_feedback = true;
                } else if frame.id == client.ids().current {
                    let state = yfoc_protocol::unpack_current_state(&frame.data);
                    println!(
                        "current Id={:+.3}A Iq={:+.3}A Iq_ref={:+.3}A Vq={:+.4}pu",
                        state.id, state.iq, state.iq_ref, state.vq
                    );
                    got_current = got_feedback;
                } else if frame.id == client.ids().ack {
                    let ack = unpack_ack(&frame.data);
                    println!(
                        "ack {} seq={} result={} state={}",
                        ack.cmd_name, ack.seq, ack.result_name, ack.state_name
                    );
                } else if frame.id == client.ids().status {
                    let state = client.parse_status(&frame.data);
                    print_status(&state);
                    if current_started && state.state == 3 {
                        return Err(format!("Current operation stopped: {}", state.fault_name));
                    }
                }
            }
            if args.once && got_feedback && (args.current.is_none() || got_current) {
                return Ok(0);
            }
        }
    })();
    let mut cleanup = Ok(());
    if current_started {
        cleanup = client.send_zero_current().map_err(|err| format!("Failed to send zero current: {err}"));
        if supports_disable {
            let disable = client.disable()
                .map(|_| println!("DISABLE acknowledged"))
                .map_err(|err| format!("Failed to confirm DISABLE: {err}"));
            cleanup = cleanup.and(disable);
        }
        if let Err(err) = &cleanup {
            eprintln!("{err}");
        }
    }
    let mut bus = client.into_bus();
    let _ = bus.shutdown();
    result.and_then(|code| cleanup.map(|_| code))
}

fn listen_loop<B: CanBus>(client: &mut Joint<B>) -> Result<i32, String> {
    let stop = Arc::new(AtomicBool::new(false));
    let flag = Arc::clone(&stop);
    let _ = ctrlc::set_handler(move || flag.store(true, Ordering::Relaxed));
    while !stop.load(Ordering::Relaxed) {
        if let Some(frame) = client.recv(Duration::from_millis(50))? {
            if frame.id == client.ids().fb {
                println!("{}", Joint::<B>::feedback_line(frame.data));
            } else if frame.id == client.ids().ack {
                let ack = unpack_ack(&frame.data);
                println!("ack {} seq={} result={} state={}", ack.cmd_name, ack.seq, ack.result_name, ack.state_name);
            }
        }
    }
    Ok(0)
}

fn print_status(status: &yfoc_protocol::Status) {
    println!(
        "status {} fault={} warn={} mode={} inner={} vbus={:.2}V temp={:.1}C",
        status.state_name,
        status.fault_name,
        yfoc_protocol::warn_name(status.warn),
        status.mode_name,
        status.inner_loop_name,
        status.vbus,
        status.temp
    );
}

fn fmt_num(value: f64) -> String {
    let text = format!("{value:.6}");
    let trimmed = text.trim_end_matches('0').trim_end_matches('.');
    if trimmed.is_empty() { "0".into() } else { trimmed.into() }
}
