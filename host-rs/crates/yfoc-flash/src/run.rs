//! 枚举探针并烧录。只擦镜像盖住的扇区，不整片擦除。

use std::collections::HashMap;
use std::path::PathBuf;
use std::thread;
use std::time::Duration;

use probe_rs::flashing::{
    DownloadOptions, FlashProgress, ProgressEvent, ProgressOperation,
};
use probe_rs::probe::list::Lister;
use probe_rs::probe::{DebugProbeInfo, WireProtocol};
use probe_rs::Permissions;

use crate::board::{attach_name, Board};
use crate::image::read_image;

/**
 * @brief 一次扫描里的一只探针。
 *
 * 同一 VID/PID/序列号出现多次时用 `ordinal` 区分，烧录时按这个重新打开。
 */
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProbeId {
    pub label: String,
    pub kind: String,
    pub vendor_id: u16,
    pub product_id: u16,
    pub serial: Option<String>,
    pub ordinal: usize,
}

/** @brief 一次烧录请求。 */
#[derive(Clone, Debug)]
pub struct FlashJob {
    pub probe: ProbeId,
    pub board: Board,
    pub image: PathBuf,
    pub verify: bool,
    pub reset_run: bool,
    pub overwrite_nv: bool,
}

/** @brief 烧录过程中的一行状态。 */
#[derive(Clone, Debug)]
pub struct Progress {
    pub phase: String,
    pub done: u64,
    pub total: u64,
    pub log: Option<String>,
}

/** @brief 列出当前系统上 probe-rs 能打开的全部探针。 */
pub fn list_probes() -> Vec<ProbeId> {
    probes_from(Lister::new().list_all())
}

/**
 * @brief 把固件写进 `job.board`。
 *
 * `on_progress` 在调用线程上同步触发。不请求整片擦除；未勾选覆盖时标定页必须留在镜像之外。
 */
pub fn flash_image(job: &FlashJob, mut on_progress: impl FnMut(Progress)) -> Result<(), String> {
    let blocks = read_image(&job.image, job.board, job.overwrite_nv)?;
    let chip = attach_name(job.board)?;
    emit(&mut on_progress, "连接", 0, 0, format!("目标 {}（{}），探针 {}", chip, job.board.label(), job.probe.label));

    let mut session = connect(job, &chip, &mut on_progress)?;
    let mut loader = session.target().flash_loader();
    for block in blocks {
        loader.add_data(block.address, &block.data).map_err(|err| format!("生成写入布局失败: {}", explain(&err)))?;
    }

    let mut totals = [0u64; 4];
    let mut done = 0u64;
    let mut shown_total = 0u64;
    let mut options = DownloadOptions::default();
    options.do_chip_erase = false;
    options.verify = job.verify;
    options.keep_unwritten_bytes = true;
    options.progress = FlashProgress::new(|event| match event {
        ProgressEvent::AddProgressBar { operation, total } => {
            totals[op_index(operation)] = total.unwrap_or(0);
        }
        ProgressEvent::Started(operation) => {
            done = 0;
            shown_total = totals[op_index(operation)];
            on_progress(Progress {
                phase: op_name(operation).into(),
                done: 0,
                total: shown_total,
                log: Some(format!("开始{}", op_name(operation))),
            });
        }
        ProgressEvent::Progress { operation, size, .. } => {
            done = done.saturating_add(size);
            shown_total = totals[op_index(operation)];
            on_progress(Progress { phase: op_name(operation).into(), done, total: shown_total, log: None });
        }
        ProgressEvent::Finished(operation) => {
            let total = totals[op_index(operation)].max(done);
            on_progress(Progress {
                phase: op_name(operation).into(),
                done: total,
                total,
                log: Some(format!("{}完成", op_name(operation))),
            });
        }
        ProgressEvent::Failed(operation) => {
            on_progress(Progress {
                phase: op_name(operation).into(),
                done,
                total: totals[op_index(operation)],
                log: Some(format!("{}失败", op_name(operation))),
            });
        }
        ProgressEvent::DiagnosticMessage { message } => {
            on_progress(Progress { phase: String::new(), done, total: shown_total, log: Some(message) });
        }
        ProgressEvent::FlashLayoutReady { .. } => {
            on_progress(Progress {
                phase: "准备".into(),
                done: 0,
                total: 0,
                log: Some("Flash 布局已生成，只擦镜像盖住的扇区".into()),
            });
        }
    });

    loader.commit(&mut session, options).map_err(|err| format!("烧录失败: {}", explain(&err)))?;

    if job.reset_run {
        emit(&mut on_progress, "复位", 0, 0, "复位并运行");
        let mut core = session.core(0).map_err(|err| format!("固件已写入，打开内核失败: {}", explain(&err)))?;
        core.reset().map_err(|err| format!("固件已写入，复位失败: {}", explain(&err)))?;
    }
    emit(&mut on_progress, "完成", 1, 1, format!("已烧录 {}", job.image.display()));
    Ok(())
}

fn connect(job: &FlashJob, chip: &str, on_progress: &mut impl FnMut(Progress)) -> Result<probe_rs::Session, String> {
    // 这几块板的 OpenOCD 配置是 reset_config none：nRESET 没有接到芯片。
    // 拉复位脚再连接会一直读到低电平，并把真正的 SWD 超时盖掉。
    let speeds = [job.board.speed_khz(), 100];
    let mut last = String::new();
    for speed in speeds {
        for attempt in 1..=2 {
            match open_and_attach(job, chip, speed) {
                Ok(session) => return Ok(session),
                Err(err) => {
                    last = err;
                    emit(
                        on_progress,
                        "连接",
                        0,
                        0,
                        format!("{speed} kHz 第 {attempt} 次连接失败（{last}）"),
                    );
                    thread::sleep(Duration::from_millis(200));
                }
            }
        }
    }
    Err(format!("连接 {chip} 失败: {last}"))
}

fn open_and_attach(job: &FlashJob, chip: &str, speed_khz: u32) -> Result<probe_rs::Session, String> {
    let mut probe = open_probe(&job.probe)?;
    if let Err(err) = probe.select_protocol(WireProtocol::Swd) {
        if probe.protocol() != Some(WireProtocol::Swd) && probe.protocol().is_some() {
            return Err(format!("这只探针不能切到 SWD: {}", explain(&err)));
        }
    }
    match probe.set_speed(speed_khz) {
        Ok(actual) => {
            if actual == 0 {
                return Err("探针没有接受 SWD 时钟".into());
            }
        }
        Err(err) => return Err(format!("设置 SWD 时钟失败: {}", explain(&err))),
    }
    probe
        .attach(chip, Permissions::new())
        .map_err(|err| format!("连接 {chip} 失败: {}", explain(&err)))
}

fn open_probe(id: &ProbeId) -> Result<probe_rs::probe::Probe, String> {
    let infos = Lister::new().list_all();
    let mut seen = HashMap::<(u16, u16, Option<String>, String), usize>::new();
    for info in &infos {
        let key = probe_key(info);
        let ordinal = seen.get(&key).copied().unwrap_or(0);
        *seen.entry(key).or_insert(0) += 1;
        if info.vendor_id == id.vendor_id
            && info.product_id == id.product_id
            && info.serial_number == id.serial
            && info.probe_type() == id.kind
            && ordinal == id.ordinal
        {
            return info.open().map_err(|err| format!("打开 {} 失败: {}", id.label, explain(&err)));
        }
    }
    Err(format!("找不到探针 {}，请重新扫描", id.label))
}

fn probes_from(infos: Vec<DebugProbeInfo>) -> Vec<ProbeId> {
    let mut seen = HashMap::<(u16, u16, Option<String>, String), usize>::new();
    let mut out = Vec::with_capacity(infos.len());
    for info in infos {
        let kind = info.probe_type();
        let key = probe_key(&info);
        let ordinal = seen.get(&key).copied().unwrap_or(0);
        *seen.entry(key).or_insert(0) += 1;
        let serial = info.serial_number.clone();
        let id_text = if info.identifier.is_empty() {
            format!("{:04X}:{:04X}", info.vendor_id, info.product_id)
        } else {
            info.identifier.clone()
        };
        let label = match &serial {
            Some(serial) if !serial.is_empty() => format!("{kind}  {id_text}  {serial}"),
            _ => format!("{kind}  {id_text}"),
        };
        out.push(ProbeId {
            label,
            kind,
            vendor_id: info.vendor_id,
            product_id: info.product_id,
            serial,
            ordinal,
        });
    }
    out
}

fn probe_key(info: &DebugProbeInfo) -> (u16, u16, Option<String>, String) {
    (info.vendor_id, info.product_id, info.serial_number.clone(), info.probe_type())
}

fn op_index(operation: ProgressOperation) -> usize {
    match operation {
        ProgressOperation::Fill => 0,
        ProgressOperation::Erase => 1,
        ProgressOperation::Program => 2,
        ProgressOperation::Verify => 3,
    }
}

fn op_name(operation: ProgressOperation) -> &'static str {
    match operation {
        ProgressOperation::Fill => "填充",
        ProgressOperation::Erase => "擦除",
        ProgressOperation::Program => "编程",
        ProgressOperation::Verify => "校验",
    }
}

fn emit(on_progress: &mut impl FnMut(Progress), phase: &str, done: u64, total: u64, log: impl Into<String>) {
    on_progress(Progress { phase: phase.into(), done, total, log: Some(log.into()) });
}

fn explain(err: &dyn std::error::Error) -> String {
    let mut text = err.to_string();
    let mut source = err.source();
    while let Some(next) = source {
        text.push_str(": ");
        text.push_str(&next.to_string());
        source = next.source();
    }
    text
}
