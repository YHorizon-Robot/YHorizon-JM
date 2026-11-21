//! 进程内回环，给会话测试用。一边 `send` 的帧从另一边 `recv`。

use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

use crate::{CanBus, CanError, Frame};

#[derive(Default)]
struct Pipe {
    frames: Mutex<VecDeque<Frame>>,
    cv: Condvar,
}

/// 成对的内存 CAN 口。
pub struct VirtualBus {
    tx: Arc<Pipe>,
    rx: Arc<Pipe>,
    closed: bool,
}

/// 造一对互连的虚拟总线。
pub fn virtual_pair() -> (VirtualBus, VirtualBus) {
    let a = Arc::new(Pipe::default());
    let b = Arc::new(Pipe::default());
    (
        VirtualBus {
            tx: Arc::clone(&a),
            rx: Arc::clone(&b),
            closed: false,
        },
        VirtualBus {
            tx: b,
            rx: a,
            closed: false,
        },
    )
}

impl CanBus for VirtualBus {
    fn send(&mut self, frame: &Frame) -> Result<(), CanError> {
        if self.closed {
            return Err(CanError::new("虚拟总线已关闭"));
        }
        self.tx.frames.lock().expect("virtual tx").push_back(*frame);
        self.tx.cv.notify_one();
        Ok(())
    }

    fn recv_timeout(&mut self, timeout: Duration) -> Result<Option<Frame>, CanError> {
        if self.closed {
            return Err(CanError::new("虚拟总线已关闭"));
        }
        let mut guard = self.rx.frames.lock().expect("virtual rx");
        let deadline = std::time::Instant::now() + timeout;
        loop {
            if let Some(frame) = guard.pop_front() {
                return Ok(Some(frame));
            }
            let now = std::time::Instant::now();
            if now >= deadline {
                return Ok(None);
            }
            let (next, wait) = self
                .rx
                .cv
                .wait_timeout(guard, deadline - now)
                .expect("virtual wait");
            guard = next;
            if wait.timed_out() && guard.is_empty() {
                return Ok(None);
            }
        }
    }

    fn shutdown(&mut self) -> Result<(), CanError> {
        self.closed = true;
        Ok(())
    }
}
