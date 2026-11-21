//! Windows 上动态加载 PCAN-Basic。没有安装驱动时，连接会说明找不到 DLL。

use std::time::{Duration, Instant};

use libloading::{Library, Symbol};

use crate::{CanBus, CanError, Frame};

const ERROR_OK: u32 = 0;
const ERROR_QRCVEMPTY: u32 = 0x20;
const ERROR_BUSLIGHT: u32 = 0x04;
const ERROR_BUSHEAVY: u32 = 0x08;
const CHANNEL_CONDITION: u8 = 0x0D;
const CHANNEL_AVAILABLE: u32 = 1;
const CHANNEL_OCCUPIED: u32 = 2;
const CHANNEL_PCANVIEW: u32 = 3;
const MSG_STANDARD: u8 = 0;

#[repr(C)]
struct TPcanMsg {
    id: u32,
    msgtype: u8,
    len: u8,
    data: [u8; 8],
}

#[repr(C)]
struct TPcanTimestamp {
    millis: u32,
    millis_overflow: u16,
    micros: u16,
}

type FnInit = unsafe extern "system" fn(u16, u16, u8, u32, u16) -> u32;
type FnUninit = unsafe extern "system" fn(u16) -> u32;
type FnRead = unsafe extern "system" fn(u16, *mut TPcanMsg, *mut TPcanTimestamp) -> u32;
type FnWrite = unsafe extern "system" fn(u16, *mut TPcanMsg) -> u32;
type FnGetValue = unsafe extern "system" fn(u16, u8, *mut u8, u32) -> u32;

/// 一个 PCAN 通道。
pub struct PcanBus {
    init: FnInit,
    uninit: FnUninit,
    read: FnRead,
    write: FnWrite,
    /// 动态库必须活到通道关闭，否则函数指针会失效。
    #[allow(dead_code)]
    lib: Library,
    handle: u16,
    alive: bool,
}

impl PcanBus {
    /// 按 `PCAN_USBBUS1` 这类名字打开。波特率使用 PCAN 预分频表。
    pub fn open(channel: &str, bitrate: u32) -> Result<Self, CanError> {
        let handle = parse_handle(channel)?;
        let baud = pcan_baud(bitrate)?;
        let lib = load_library()?;
        let init: FnInit = load_sym(&lib, b"CAN_Initialize")?;
        let uninit: FnUninit = load_sym(&lib, b"CAN_Uninitialize")?;
        let read: FnRead = load_sym(&lib, b"CAN_Read")?;
        let write: FnWrite = load_sym(&lib, b"CAN_Write")?;
        let status = unsafe { init(handle, baud, 0, 0, 0) };
        if status != ERROR_OK {
            return Err(CanError::new(format!(
                "CAN_Initialize({channel}) 失败，代码 0x{status:X}。确认 PCAN 已插上且没有被别的程序占用"
            )));
        }
        Ok(Self {
            init,
            uninit,
            read,
            write,
            lib,
            handle,
            alive: true,
        })
    }
}

impl CanBus for PcanBus {
    fn send(&mut self, frame: &Frame) -> Result<(), CanError> {
        if !self.alive {
            return Err(CanError::new("PCAN 已关闭"));
        }
        let mut msg = TPcanMsg {
            id: frame.id,
            msgtype: MSG_STANDARD,
            len: 8,
            data: frame.data,
        };
        let status = unsafe { (self.write)(self.handle, &mut msg) };
        if status != ERROR_OK {
            return Err(CanError::new(format!("CAN_Write 失败，代码 0x{status:X}")));
        }
        Ok(())
    }

    fn recv_timeout(&mut self, timeout: Duration) -> Result<Option<Frame>, CanError> {
        if !self.alive {
            return Err(CanError::new("PCAN 已关闭"));
        }
        let deadline = Instant::now() + timeout;
        loop {
            let mut msg = TPcanMsg {
                id: 0,
                msgtype: 0,
                len: 0,
                data: [0; 8],
            };
            let mut stamp = TPcanTimestamp {
                millis: 0,
                millis_overflow: 0,
                micros: 0,
            };
            let status = unsafe { (self.read)(self.handle, &mut msg, &mut stamp) };
            if status == ERROR_QRCVEMPTY || status == ERROR_BUSLIGHT || status == ERROR_BUSHEAVY {
                if status != ERROR_QRCVEMPTY && msg.len == 8 && msg.msgtype == MSG_STANDARD {
                    return Ok(Some(Frame {
                        id: msg.id,
                        data: msg.data,
                    }));
                }
                if Instant::now() >= deadline {
                    return Ok(None);
                }
                std::thread::sleep(Duration::from_millis(1).min(timeout.max(Duration::from_millis(1))));
                continue;
            }
            if status != ERROR_OK {
                return Err(CanError::new(format!("CAN_Read 失败，代码 0x{status:X}")));
            }
            if msg.msgtype == MSG_STANDARD && msg.len == 8 && msg.id <= 0x7FF {
                return Ok(Some(Frame {
                    id: msg.id,
                    data: msg.data,
                }));
            }
            if Instant::now() >= deadline {
                return Ok(None);
            }
        }
    }

    fn shutdown(&mut self) -> Result<(), CanError> {
        if self.alive {
            unsafe { (self.uninit)(self.handle) };
            self.alive = false;
        }
        Ok(())
    }
}

impl Drop for PcanBus {
    fn drop(&mut self) {
        let _ = self.shutdown();
    }
}

fn load_library() -> Result<Library, CanError> {
    unsafe { Library::new("PCANBasic.dll") }.map_err(|_| {
        CanError::new(
            "找不到 PCANBasic.dll。请先安装 PEAK PCAN-Basic，并确认它在 PATH 或系统目录里",
        )
    })
}

fn load_sym<T>(lib: &Library, name: &[u8]) -> Result<T, CanError>
where
    T: Copy,
{
    let sym: Symbol<T> = unsafe { lib.get(name) }.map_err(|err| {
        CanError::new(format!(
            "PCANBasic.dll 缺少 {}: {err}",
            String::from_utf8_lossy(name)
        ))
    })?;
    Ok(*sym)
}

fn parse_handle(channel: &str) -> Result<u16, CanError> {
    let text = channel.trim().to_ascii_uppercase();
    if let Some(rest) = text.strip_prefix("PCAN_USBBUS") {
        if let Ok(n) = rest.parse::<u16>() {
            if (1..=16).contains(&n) {
                return Ok(0x50 + n);
            }
        }
    }
    Err(CanError::new(format!(
        "无法识别 PCAN 通道 {channel}。请使用 PCAN_USBBUS1 这种名字"
    )))
}

fn pcan_baud(bitrate: u32) -> Result<u16, CanError> {
    match bitrate {
        1_000_000 => Ok(0x0014),
        500_000 => Ok(0x001C),
        250_000 => Ok(0x011C),
        125_000 => Ok(0x031C),
        100_000 => Ok(0x432F),
        other => Err(CanError::new(format!("PCAN 预设表没有波特率 {other}"))),
    }
}

/// 用通道状态查询哪些 USB 通道插着设备。DLL 不在时返回空。
pub fn detect() -> Vec<String> {
    let Ok(lib) = load_library() else {
        return Vec::new();
    };
    let Ok(get): Result<FnGetValue, _> = load_sym(&lib, b"CAN_GetValue") else {
        return Vec::new();
    };
    let mut found = Vec::new();
    for n in 1..=8u16 {
        let handle = 0x50 + n;
        let mut cond = 0u32;
        let status = unsafe {
            get(
                handle,
                CHANNEL_CONDITION,
                &mut cond as *mut u32 as *mut u8,
                4,
            )
        };
        if status == ERROR_OK
            && matches!(cond, CHANNEL_AVAILABLE | CHANNEL_OCCUPIED | CHANNEL_PCANVIEW)
        {
            found.push(format!("PCAN_USBBUS{n}"));
        }
    }
    found
}

#[allow(dead_code)]
fn _keep_init(bus: &PcanBus) -> FnInit {
    bus.init
}
