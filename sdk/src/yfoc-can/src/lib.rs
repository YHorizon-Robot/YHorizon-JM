//! 经典 CAN 后端。
//!
//! Windows 使用 PCAN-Basic 动态库，Linux 使用 SocketCAN，三个平台都可用 slcan。
//! `usb2can`、Kvaser、Vector 只出现在界面名单里，连接时会明确报未实现。

mod pcan;
mod slcan;
#[cfg(target_os = "linux")]
mod socketcan_bus;
mod virtual_bus;

use std::fmt;
use std::time::Duration;

pub use virtual_bus::{virtual_pair, VirtualBus};

/// 一帧经典 CAN，固定 8 字节数据。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Frame {
    /// 标准仲裁 ID，11 位。
    pub id: u32,
    /// 数据。
    pub data: [u8; 8],
}

/// 打开或收发失败。
#[derive(Debug)]
pub struct CanError {
    /// 给人看的原因。
    pub message: String,
}

impl CanError {
    /// 由文字构造错误。
    pub fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }
}

impl fmt::Display for CanError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for CanError {}

/// 可收发标准帧的总线。
pub trait CanBus: Send {
    /// 发送一帧。调用返回时帧已交给适配器。
    fn send(&mut self, frame: &Frame) -> Result<(), CanError>;
    /// 等到一帧或超时。超时返回 `Ok(None)`。
    fn recv_timeout(&mut self, timeout: Duration) -> Result<Option<Frame>, CanError>;
    /// 关闭适配器。重复调用应无害。
    fn shutdown(&mut self) -> Result<(), CanError>;
}

/// 已实现的后端。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Backend {
    /// PEAK PCAN-Basic。仅 Windows 能真正打开。
    Pcan,
    /// Linux SocketCAN。波特率由系统接口决定。
    Socketcan,
    /// Lawicel 串行 CAN。
    Slcan,
}

impl Backend {
    /// 界面和 CLI 使用的名字，与 python-can 的接口名一致。
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Pcan => "pcan",
            Self::Socketcan => "socketcan",
            Self::Slcan => "slcan",
        }
    }

    /// 解析接口名。未实现的名字返回 `None`，由调用方给出明确错误。
    pub fn parse(name: &str) -> Option<Self> {
        match name.trim().to_ascii_lowercase().as_str() {
            "pcan" => Some(Self::Pcan),
            "socketcan" => Some(Self::Socketcan),
            "slcan" => Some(Self::Slcan),
            _ => None,
        }
    }
}

/// 界面里列出、但本版不打开的接口。
pub const UNIMPLEMENTED: &[&str] = &["usb2can", "kvaser", "vector"];

/// 可选波特率。
pub const BITRATES: &[u32] = &[1_000_000, 500_000, 250_000, 125_000, 100_000];

/// 当前系统默认接口。
pub fn default_backend() -> Backend {
    if cfg!(windows) {
        Backend::Pcan
    } else if cfg!(target_os = "linux") {
        Backend::Socketcan
    } else {
        Backend::Slcan
    }
}

/// 界面下拉用的接口名。未实现的排在后面。
pub fn interface_names() -> Vec<String> {
    let mut names = Vec::new();
    if cfg!(windows) {
        names.push("pcan".into());
        names.push("slcan".into());
    } else if cfg!(target_os = "linux") {
        names.push("socketcan".into());
        names.push("pcan".into());
        names.push("slcan".into());
    } else {
        names.push("slcan".into());
        names.push("pcan".into());
    }
    for name in UNIMPLEMENTED {
        names.push((*name).into());
    }
    names
}

/// 这个名字能否被 [`open`] 打开。
pub fn is_implemented(name: &str) -> bool {
    Backend::parse(name).is_some()
}

/// 打开总线。SocketCAN 忽略 `bitrate`，需要事先用 ip 设好。
pub fn open(interface: &str, channel: &str, bitrate: u32) -> Result<Box<dyn CanBus>, CanError> {
    let Some(backend) = Backend::parse(interface) else {
        return Err(CanError::new(format!(
            "{interface} 尚未实现。本版支持 pcan、socketcan 和 slcan"
        )));
    };
    match backend {
        Backend::Pcan => {
            #[cfg(windows)]
            {
                Ok(Box::new(pcan::PcanBus::open(channel, bitrate)?))
            }
            #[cfg(not(windows))]
            {
                let _ = (channel, bitrate);
                Err(CanError::new(
                    "PCAN-Basic 动态加载仅在 Windows 上提供。Linux 请用 socketcan，macOS 请用 slcan",
                ))
            }
        }
        Backend::Socketcan => {
            #[cfg(target_os = "linux")]
            {
                let _ = bitrate;
                Ok(Box::new(socketcan_bus::SocketCanBus::open(channel)?))
            }
            #[cfg(not(target_os = "linux"))]
            {
                let _ = (channel, bitrate);
                Err(CanError::new(
                    "socketcan 只存在于 Linux。Windows 请用 pcan 或 slcan",
                ))
            }
        }
        Backend::Slcan => Ok(Box::new(slcan::SlcanBus::open(channel, bitrate)?)),
    }
}

/// 探测当前插着的通道。失败时返回空列表，不把探测错误当成没有设备以外的崩溃。
pub fn detect_channels(interface: &str) -> Vec<String> {
    match Backend::parse(interface) {
        Some(Backend::Pcan) => pcan::detect(),
        Some(Backend::Socketcan) => {
            #[cfg(target_os = "linux")]
            {
                socketcan_bus::detect()
            }
            #[cfg(not(target_os = "linux"))]
            {
                Vec::new()
            }
        }
        Some(Backend::Slcan) => slcan::detect(),
        None => Vec::new(),
    }
}

/// 某接口的默认通道。优先用探测结果。
pub fn default_channel(interface: &str) -> String {
    let found = detect_channels(interface);
    if let Some(first) = found.into_iter().next() {
        return first;
    }
    preset_channels(interface)
        .into_iter()
        .next()
        .unwrap_or_default()
}

/// 下拉列表：探测到的通道在前，常用预设补在后面。
pub fn list_channels(interface: &str) -> Vec<String> {
    let mut out = Vec::new();
    for channel in detect_channels(interface).into_iter().chain(preset_channels(interface)) {
        if !channel.is_empty() && !out.contains(&channel) {
            out.push(channel);
        }
    }
    out
}

impl<T: CanBus + ?Sized> CanBus for Box<T> {
    fn send(&mut self, frame: &Frame) -> Result<(), CanError> {
        (**self).send(frame)
    }

    fn recv_timeout(&mut self, timeout: Duration) -> Result<Option<Frame>, CanError> {
        (**self).recv_timeout(timeout)
    }

    fn shutdown(&mut self) -> Result<(), CanError> {
        (**self).shutdown()
    }
}

/// 借用同一个适配器可依次访问不同节点，或在查询节点后将总线交给组同步。
impl<T: CanBus + ?Sized> CanBus for &mut T {
    fn send(&mut self, frame: &Frame) -> Result<(), CanError> {
        (**self).send(frame)
    }

    fn recv_timeout(&mut self, timeout: Duration) -> Result<Option<Frame>, CanError> {
        (**self).recv_timeout(timeout)
    }

    fn shutdown(&mut self) -> Result<(), CanError> {
        (**self).shutdown()
    }
}

fn preset_channels(interface: &str) -> Vec<String> {
    match interface.trim().to_ascii_lowercase().as_str() {
        "pcan" => (1..=8).map(|i| format!("PCAN_USBBUS{i}")).collect(),
        "socketcan" => {
            let mut names: Vec<String> = (0..4).map(|i| format!("can{i}")).collect();
            names.push("vcan0".into());
            names
        }
        "slcan" => slcan::preset_ports(),
        "usb2can" | "kvaser" | "vector" => vec!["0".into(), "1".into()],
        _ => Vec::new(),
    }
}
