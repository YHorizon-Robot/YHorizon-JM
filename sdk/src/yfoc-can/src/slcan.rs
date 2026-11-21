//! Lawicel（slcan）串行 CAN。串口速率固定 115200 8N1，CAN 波特率用 `S` 命令设置。

use std::io::{ErrorKind, Read, Write};
use std::time::{Duration, Instant};

use serialport::{SerialPort, SerialPortType};

use crate::{CanBus, CanError, Frame};

/// slcan 总线。
pub struct SlcanBus {
    port: Box<dyn SerialPort>,
    buf: Vec<u8>,
    open: bool,
}

impl SlcanBus {
    /// 打开串口并发送 `C` / `S` / `O`。
    pub fn open(channel: &str, bitrate: u32) -> Result<Self, CanError> {
        let speed = slcan_speed(bitrate)?;
        let port = serialport::new(channel, 115_200)
            .timeout(Duration::from_millis(200))
            .open()
            .map_err(|err| CanError::new(format!("打开 slcan {channel} 失败: {err}")))?;
        let mut bus = Self {
            port,
            buf: Vec::new(),
            open: true,
        };
        // 有些适配器在已经关闭时会用 BEL 回答 C，这里忽略。
        let _ = bus.command(b"C\r");
        bus.command(format!("S{speed}\r").as_bytes())?;
        bus.command(b"O\r")?;
        let _ = bus.port.set_timeout(Duration::from_millis(1));
        Ok(bus)
    }

    fn command(&mut self, bytes: &[u8]) -> Result<(), CanError> {
        self.port
            .write_all(bytes)
            .map_err(|err| CanError::new(format!("slcan 写入失败: {err}")))?;
        self.port
            .flush()
            .map_err(|err| CanError::new(format!("slcan 刷新失败: {err}")))?;
        let deadline = Instant::now() + Duration::from_millis(300);
        let mut tmp = [0u8; 64];
        while Instant::now() < deadline {
            match self.port.read(&mut tmp) {
                Ok(0) => {}
                Ok(n) => {
                    if tmp[..n].contains(&0x07) {
                        return Err(CanError::new("slcan 适配器拒绝了打开命令"));
                    }
                    if tmp[..n].contains(&b'\r') {
                        return Ok(());
                    }
                }
                Err(err) if err.kind() == ErrorKind::TimedOut => {}
                Err(err) => return Err(CanError::new(format!("slcan 读应答失败: {err}"))),
            }
        }
        Err(CanError::new("slcan 打开命令超时"))
    }

    fn pop_frame(&mut self) -> Option<Frame> {
        while let Some(end) = self.buf.iter().position(|b| *b == b'\r') {
            let line: Vec<u8> = self.buf.drain(..=end).collect();
            if let Some(frame) = parse_line(&line[..line.len().saturating_sub(1)]) {
                return Some(frame);
            }
        }
        if self.buf.len() > 4096 {
            self.buf.clear();
        }
        None
    }
}

impl CanBus for SlcanBus {
    fn send(&mut self, frame: &Frame) -> Result<(), CanError> {
        if !self.open {
            return Err(CanError::new("slcan 已关闭"));
        }
        if frame.id > 0x7FF {
            return Err(CanError::new("slcan 只发送标准帧"));
        }
        let mut line = format!("t{:03X}8", frame.id);
        for byte in frame.data {
            line.push_str(&format!("{byte:02X}"));
        }
        line.push('\r');
        self.port
            .write_all(line.as_bytes())
            .map_err(|err| CanError::new(format!("slcan 发送失败: {err}")))?;
        Ok(())
    }

    fn recv_timeout(&mut self, timeout: Duration) -> Result<Option<Frame>, CanError> {
        if let Some(frame) = self.pop_frame() {
            return Ok(Some(frame));
        }
        if timeout.is_zero() {
            let waiting = self.port.bytes_to_read().unwrap_or(0);
            if waiting == 0 {
                return Ok(None);
            }
        }
        let _ = self.port.set_timeout(timeout.max(Duration::from_millis(1)));
        let mut tmp = [0u8; 256];
        match self.port.read(&mut tmp) {
            Ok(n) => self.buf.extend_from_slice(&tmp[..n]),
            Err(err) if err.kind() == ErrorKind::TimedOut => {}
            Err(err) => return Err(CanError::new(format!("slcan 接收失败: {err}"))),
        }
        Ok(self.pop_frame())
    }

    fn shutdown(&mut self) -> Result<(), CanError> {
        if self.open {
            let _ = self.port.write_all(b"C\r");
            self.open = false;
        }
        Ok(())
    }
}

impl Drop for SlcanBus {
    fn drop(&mut self) {
        let _ = self.shutdown();
    }
}

fn parse_line(line: &[u8]) -> Option<Frame> {
    if line.first().copied() != Some(b't') || line.len() < 5 {
        return None;
    }
    let id = u32::from_str_radix(std::str::from_utf8(&line[1..4]).ok()?, 16).ok()?;
    let dlc = (line[4] as char).to_digit(16)? as usize;
    if dlc != 8 || line.len() < 5 + dlc * 2 || id > 0x7FF {
        return None;
    }
    let mut data = [0u8; 8];
    for (i, slot) in data.iter_mut().enumerate() {
        let at = 5 + i * 2;
        *slot = u8::from_str_radix(std::str::from_utf8(&line[at..at + 2]).ok()?, 16).ok()?;
    }
    Some(Frame { id, data })
}

fn slcan_speed(bitrate: u32) -> Result<u8, CanError> {
    match bitrate {
        10_000 => Ok(0),
        20_000 => Ok(1),
        50_000 => Ok(2),
        100_000 => Ok(3),
        125_000 => Ok(4),
        250_000 => Ok(5),
        500_000 => Ok(6),
        800_000 => Ok(7),
        1_000_000 => Ok(8),
        other => Err(CanError::new(format!("slcan 不支持波特率 {other}"))),
    }
}

/// 当前串口。探测失败时退回常用端口名。
pub fn detect() -> Vec<String> {
    match serialport::available_ports() {
        Ok(ports) => ports
            .into_iter()
            .map(|port| port.port_name)
            .filter(|name| !name.is_empty())
            .collect(),
        Err(_) => Vec::new(),
    }
}

/// 没有枚举到串口时给界面的占位。
pub fn preset_ports() -> Vec<String> {
    let found = detect();
    if !found.is_empty() {
        return found;
    }
    if cfg!(windows) {
        vec!["COM3".into(), "COM4".into()]
    } else if cfg!(target_os = "macos") {
        vec!["/dev/cu.usbserial".into(), "/dev/cu.usbmodem".into()]
    } else {
        vec!["/dev/ttyACM0".into(), "/dev/ttyUSB0".into()]
    }
}

#[allow(dead_code)]
fn _port_kind(kind: &SerialPortType) -> bool {
    matches!(kind, SerialPortType::UsbPort(_))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_standard_frame() {
        let line = b"t12380102030405060708";
        let frame = parse_line(line).unwrap();
        assert_eq!(frame.id, 0x123);
        assert_eq!(frame.data, [1, 2, 3, 4, 5, 6, 7, 8]);
    }
}
