//! Linux SocketCAN。波特率要事先用 `ip link` 配好，这里不再改。

use std::io::ErrorKind;
use std::time::Duration;

use socketcan::{CanFrame, CanSocket, EmbeddedFrame, Socket, StandardId};

use crate::{CanBus, CanError, Frame};

/// 一个 SocketCAN 网络接口。
pub struct SocketCanBus {
    socket: CanSocket,
}

impl SocketCanBus {
    /// 打开 `can0` 这类接口。
    pub fn open(channel: &str) -> Result<Self, CanError> {
        let socket = CanSocket::open(channel)
            .map_err(|err| CanError::new(format!("打开 SocketCAN {channel} 失败: {err}")))?;
        Ok(Self { socket })
    }
}

impl CanBus for SocketCanBus {
    fn send(&mut self, frame: &Frame) -> Result<(), CanError> {
        let id = u16::try_from(frame.id).ok().and_then(StandardId::new)
            .ok_or_else(|| CanError::new(format!("非法标准帧 ID 0x{:X}", frame.id)))?;
        let message = CanFrame::new(id, &frame.data)
            .ok_or_else(|| CanError::new("构造 CAN 帧失败"))?;
        self.socket
            .write_frame(&message)
            .map_err(|err| CanError::new(format!("SocketCAN 发送失败: {err}")))
    }

    fn recv_timeout(&mut self, timeout: Duration) -> Result<Option<Frame>, CanError> {
        match self.socket.read_frame_timeout(timeout) {
            Ok(frame) => Ok(classic_frame(frame)),
            Err(err) if err.kind() == ErrorKind::WouldBlock || err.kind() == ErrorKind::TimedOut => {
                Ok(None)
            }
            Err(err) => Err(CanError::new(format!("SocketCAN 接收失败: {err}"))),
        }
    }

    fn shutdown(&mut self) -> Result<(), CanError> {
        Ok(())
    }
}

fn classic_frame(frame: CanFrame) -> Option<Frame> {
    let CanFrame::Data(frame) = frame else { return None };
    let socketcan::Id::Standard(id) = frame.id() else {
        return None;
    };
    if frame.data().len() != 8 {
        return None;
    }
    let mut data = [0u8; 8];
    data.copy_from_slice(frame.data());
    Some(Frame {
        id: id.as_raw() as u32,
        data,
    })
}

/// 读 `/sys/class/net`，类型 280 是 CAN。
pub fn detect() -> Vec<String> {
    let mut found = Vec::new();
    let Ok(entries) = std::fs::read_dir("/sys/class/net") else {
        return found;
    };
    for entry in entries.flatten() {
        let path = entry.path().join("type");
        if std::fs::read_to_string(path).map(|text| text.trim() == "280").unwrap_or(false) {
            found.push(entry.file_name().to_string_lossy().into_owned());
        }
    }
    found.sort();
    found
}
