//! C ABI used by sdk/include and sdk/python. All foreign pointers must remain
//! valid for the duration of a synchronous call. Bus handles are registry IDs.
use crate::{can, multi_axis, protocol as p, ControlMode, Joint};
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

type Result<T> = std::result::Result<T, String>;
type Motor = Joint<Box<dyn can::CanBus>>;
struct Bus {
    motor: Motor,
    info: Option<p::DeviceInfo>,
    bitrate: u32,
}
struct Session {
    bus: Mutex<Option<Bus>>,
    cancel: AtomicBool,
}
type Registry = Mutex<HashMap<u64, Arc<Session>>>;
fn registry() -> &'static Registry {
    static REGISTRY: OnceLock<Registry> = OnceLock::new();
    REGISTRY.get_or_init(Default::default)
}
fn session(id: u64) -> Result<Arc<Session>> {
    registry()
        .lock()
        .map_err(|_| "SDK registry unavailable")?
        .get(&id)
        .cloned()
        .ok_or_else(|| "CAN handle closed or invalid".into())
}
impl Session {
    fn access<T>(&self, f: impl FnOnce(&mut Bus) -> Result<T>) -> Result<T> {
        let mut guard = self.bus.try_lock().map_err(|_| "CAN connection busy")?;
        f(guard.as_mut().ok_or("CAN connection closed")?)
    }
}
impl Bus {
    fn select(&mut self, node: i32) -> Result<()> {
        if !(1..=63).contains(&node) {
            return Err("node must be 1..63".into());
        }
        let n = node as u8;
        if self.motor.ids() != p::ids(n) {
            self.motor.select_node(n);
            self.info = None;
        }
        Ok(())
    }
    fn discover(&mut self) -> Result<&p::DeviceInfo> {
        self.info = self.motor.discover()?;
        self.info
            .as_ref()
            .ok_or_else(|| "device did not answer discovery".into())
    }
}
thread_local! { static ERROR: RefCell<CString> = RefCell::new(CString::default()); }
fn api(f: impl FnOnce() -> Result<i32>) -> i32 {
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(f))
        .unwrap_or_else(|_| Err("internal SDK panic".into()));
    r.unwrap_or_else(|e| {
        ERROR.with(|s| *s.borrow_mut() = CString::new(e.replace('\0', " ")).unwrap_or_default());
        -1
    })
}
#[no_mangle]
pub extern "C" fn yfoc_abi_version() -> u32 {
    1
}
#[no_mangle]
pub extern "C" fn yfoc_last_error() -> *const c_char {
    ERROR.with(|s| s.borrow().as_ptr())
}

unsafe fn string<'a>(ptr: *const c_char) -> Result<&'a str> {
    if ptr.is_null() {
        return Err("null string".into());
    }
    CStr::from_ptr(ptr)
        .to_str()
        .map_err(|_| "expected UTF-8".into())
}
fn insert(bus: Box<dyn can::CanBus>, bitrate: u32) -> Result<u64> {
    static NEXT: AtomicU64 = AtomicU64::new(1);
    let id = NEXT.fetch_add(1, Ordering::Relaxed);
    let bus = Bus {
        motor: Joint::new(bus, 1)?,
        info: None,
        bitrate,
    };
    registry()
        .lock()
        .map_err(|_| "SDK registry unavailable")?
        .insert(
            id,
            Arc::new(Session {
                bus: Mutex::new(Some(bus)),
                cancel: AtomicBool::new(false),
            }),
        );
    Ok(id)
}
#[no_mangle]
pub unsafe extern "C" fn yfoc_open(
    interface: *const c_char,
    channel: *const c_char,
    bitrate: u32,
    out: *mut u64,
) -> i32 {
    api(|| {
        if out.is_null() {
            return Err("null handle output".into());
        }
        out.write(0);
        let bus =
            can::open(string(interface)?, string(channel)?, bitrate).map_err(|e| e.to_string())?;
        out.write(insert(bus, bitrate)?);
        Ok(0)
    })
}
#[no_mangle]
pub extern "C" fn yfoc_close(id: u64) -> i32 {
    api(|| {
        let s = session(id)?;
        let mut guard = s.bus.try_lock().map_err(|_| "CAN connection busy")?;
        let bus = guard.as_mut().ok_or("CAN connection closed")?;
        bus.motor.bus_mut().shutdown().map_err(|e| e.to_string())?;
        guard.take();
        registry()
            .lock()
            .map_err(|_| "SDK registry unavailable")?
            .remove(&id);
        Ok(0)
    })
}

#[repr(C)]
pub struct Info {
    board_id: u32,
    caps: u32,
    fw_major: u32,
    fw_minor: u32,
}
#[repr(C)]
pub struct Status {
    state: u32,
    fault: u32,
    warn: u32,
    mode: u32,
    inner_loop: i32,
    vbus: f64,
    temp: f64,
}
#[repr(C)]
pub struct Feedback {
    position: f64,
    velocity: f64,
    torque: f64,
}
impl From<p::Feedback> for Feedback {
    fn from(s: p::Feedback) -> Self {
        Self {
            position: s.pos,
            velocity: s.vel,
            torque: s.torque,
        }
    }
}
#[repr(C)]
pub struct Current {
    id: f64,
    iq: f64,
    iq_ref: f64,
    vq: f64,
}
#[repr(C)]
pub struct Config {
    i_limit: f64,
    v_limit: f64,
    kp: f64,
    ki: f64,
    kt: f64,
    gear: f64,
    bandwidth_hz: u32,
    loop_hz: u32,
}

fn node<T>(id: u64, n: i32, f: impl FnOnce(&mut Bus) -> Result<T>) -> Result<T> {
    session(id)?.access(|b| {
        b.select(n)?;
        f(b)
    })
}
unsafe fn read<T>(id: u64, n: i32, out: *mut T, f: impl FnOnce(&mut Bus) -> Result<T>) -> i32 {
    api(|| {
        if out.is_null() {
            return Err("null output".into());
        }
        out.write(node(id, n, f)?);
        Ok(0)
    })
}
impl Bus {
    fn identified(&mut self) -> Result<&mut Motor> {
        if self.info.is_none() {
            self.discover()?;
        }
        Ok(&mut self.motor)
    }
}
#[no_mangle]
pub unsafe extern "C" fn yfoc_info(id: u64, n: i32, out: *mut Info) -> i32 {
    read(id, n, out, |b| {
        let s = b.discover()?;
        Ok(Info {
            board_id: s.board_id.into(),
            caps: s.caps.into(),
            fw_major: s.fw_major.into(),
            fw_minor: s.fw_minor.into(),
        })
    })
}
#[no_mangle]
pub unsafe extern "C" fn yfoc_status(id: u64, n: i32, out: *mut Status) -> i32 {
    read(id, n, out, |b| {
        let s = b.identified()?.status()?;
        Ok(Status {
            state: s.state.into(),
            fault: s.fault.into(),
            warn: s.warn.into(),
            mode: s.mode.into(),
            inner_loop: s.inner_loop.map_or(-1, i32::from),
            vbus: s.vbus,
            temp: s.temp,
        })
    })
}
#[no_mangle]
pub unsafe extern "C" fn yfoc_feedback(id: u64, n: i32, out: *mut Feedback) -> i32 {
    read(id, n, out, |b| Ok(b.motor.feedback()?.into()))
}
#[no_mangle]
pub unsafe extern "C" fn yfoc_wait_feedback(id: u64, n: i32, ms: u32, out: *mut Feedback) -> i32 {
    api(|| {
        if out.is_null() {
            return Err("null output".into());
        }
        match node(id, n, |b| {
            b.motor.recv_feedback(Duration::from_millis(ms.into()))
        })? {
            Some(s) => {
                out.write(s.into());
                Ok(0)
            }
            None => Ok(1),
        }
    })
}
#[no_mangle]
pub unsafe extern "C" fn yfoc_current(id: u64, n: i32, out: *mut Current) -> i32 {
    read(id, n, out, |b| {
        let s = b.identified()?.current_state()?;
        Ok(Current {
            id: s.id,
            iq: s.iq,
            iq_ref: s.iq_ref,
            vq: s.vq,
        })
    })
}
#[no_mangle]
pub unsafe extern "C" fn yfoc_current_config(id: u64, n: i32, out: *mut Config) -> i32 {
    read(id, n, out, |b| {
        let c = b.identified()?.current_config()?;
        Ok(Config {
            i_limit: c.i_limit.ok_or("missing I limit")?,
            v_limit: c.v_limit.ok_or("missing V limit")?,
            kp: c.kp.ok_or("missing Kp")?,
            ki: c.ki.ok_or("missing Ki")?,
            kt: c.kt.ok_or("missing Kt")?,
            gear: c.gear.ok_or("missing gear ratio")?,
            bandwidth_hz: c.bandwidth_hz.ok_or("missing bandwidth")?.into(),
            loop_hz: c.loop_hz.ok_or("missing loop rate")?.into(),
        })
    })
}
fn command(id: u64, n: i32, f: impl FnOnce(&mut Motor) -> Result<()>) -> i32 {
    api(|| node(id, n, |b| f(&mut b.motor)).map(|_| 0))
}
#[no_mangle]
pub extern "C" fn yfoc_set_mode(id: u64, n: i32, mode: i32) -> i32 {
    command(id, n, |m| {
        m.set_mode(match mode {
            0 => ControlMode::Motion,
            1 => ControlMode::Velocity,
            2 => ControlMode::Position,
            _ => return Err("invalid control mode".into()),
        })
    })
}
#[no_mangle]
pub extern "C" fn yfoc_set_position(id: u64, n: i32, rad: f64, max_rad_s: f64) -> i32 {
    command(id, n, |m| m.set_position(rad, max_rad_s))
}
#[no_mangle]
pub extern "C" fn yfoc_set_velocity(id: u64, n: i32, rad_s: f64) -> i32 {
    command(id, n, |m| m.set_velocity(rad_s))
}
#[no_mangle]
pub extern "C" fn yfoc_disable(id: u64, n: i32) -> i32 {
    command(id, n, Motor::disable)
}

#[no_mangle]
pub extern "C" fn yfoc_set_inner(id: u64, n: i32, mode: i32) -> i32 {
    api(|| {
        node(id, n, |b| {
            let current = match mode {
                0 => false,
                1 => true,
                _ => return Err("invalid inner loop".into()),
            };
            b.identified()?.set_inner(current)
        })
        .map(|_| 0)
    })
}
#[no_mangle]
pub extern "C" fn yfoc_motion_setup(id: u64, n: i32, kp: f64, kd: f64) -> i32 {
    command(id, n, |m| {
        if !(0.0..=f64::from(u16::MAX) * p::KP_LSB_MOTION).contains(&kp)
            || !(0.0..=f64::from(u16::MAX) * p::KD_LSB_MOTION).contains(&kd)
        {
            return Err("invalid motion gains".into());
        }
        m.motion_setup(kp, kd)
    })
}
#[no_mangle]
pub extern "C" fn yfoc_set_motion(id: u64, n: i32, pos: f64, vel: f64, torque: f64) -> i32 {
    command(id, n, |m| {
        if !(f64::from(i32::MIN) * p::POS_LSB..=f64::from(i32::MAX) * p::POS_LSB).contains(&pos)
            || !(f64::from(i16::MIN) * p::VEL_LSB..=f64::from(i16::MAX) * p::VEL_LSB).contains(&vel)
            || !(f64::from(-i16::MAX) * p::TORQUE_LSB..=f64::from(i16::MAX) * p::TORQUE_LSB)
                .contains(&torque)
        {
            return Err("motion command outside wire range".into());
        }
        m.send(m.ids().motion, p::pack_motion(pos, vel, torque))
    })
}

#[repr(C)]
pub struct Target {
    node: i32,
    has_second: i32,
    target: f64,
    second: f64,
}
#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct Sample {
    host_us: u64,
    cycle: u32,
    node: u32,
    reference: f64,
    position: f64,
    velocity: f64,
}
type Callback = extern "C" fn(*mut c_void, *const Sample, usize) -> i32;

#[no_mangle]
pub unsafe extern "C" fn yfoc_run_group(
    id: u64,
    targets: *const Target,
    count: usize,
    seconds: f64,
    cb: Option<Callback>,
    ctx: *mut c_void,
) -> i32 {
    api(|| {
        if !(1..=multi_axis::MAX_AXES).contains(&count) || targets.is_null() {
            return Err("expected 1..16 axes".into());
        }
        let mut axes = Vec::with_capacity(count);
        for t in std::slice::from_raw_parts(targets, count) {
            let node = u8::try_from(t.node).map_err(|_| "invalid node")?;
            let second = match t.has_second {
                0 => None,
                1 => Some(t.second),
                _ => return Err("invalid second target flag".into()),
            };
            axes.push(multi_axis::Axis {
                node,
                target: t.target,
                second,
            });
        }
        let s = session(id)?;
        s.access(|b| {
            if b.bitrate != 1_000_000 {
                return Err("group motion requires 1 Mbit/s CAN".into());
            }
            s.cancel.store(false, Ordering::Release);
            multi_axis::execute(
                b.motor.bus_mut().as_mut(),
                &multi_axis::Plan { axes, seconds },
                &s.cancel,
                |event| {
                    if let (Some(callback), multi_axis::Event::Cycle(row)) = (cb, event) {
                        let mut samples = [Sample::default(); multi_axis::MAX_AXES];
                        for (dest, a) in samples.iter_mut().zip(&row) {
                            *dest = Sample {
                                host_us: a.host_us,
                                cycle: a.cycle.into(),
                                node: a.node.into(),
                                reference: a.reference,
                                position: a.position,
                                velocity: a.velocity,
                            };
                        }
                        if callback(ctx, samples.as_ptr(), row.len()) != 0 {
                            s.cancel.store(true, Ordering::Release);
                        }
                    }
                },
            )?;
            Ok(0)
        })
    })
}
#[no_mangle]
pub extern "C" fn yfoc_cancel_group(id: u64) -> i32 {
    api(|| {
        session(id)?.cancel.store(true, Ordering::Release);
        Ok(0)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use can::CanBus;

    #[test]
    fn shared_bus_routes_each_node_and_rejects_invalid_or_closed_handles() {
        let (bus, mut peer) = can::virtual_pair();
        let id = insert(Box::new(bus), 1_000_000).unwrap();
        assert_eq!(yfoc_set_position(id, 7, 0.1234, 1.2), 0);
        assert_eq!(yfoc_set_velocity(id, 63, -0.5), 0);
        let first = peer.recv_timeout(Duration::ZERO).unwrap().unwrap();
        assert_eq!(
            (first.id, first.data),
            (p::ids(7).pos, p::pack_position(0.1234, 1.2))
        );
        let second = peer.recv_timeout(Duration::ZERO).unwrap().unwrap();
        assert_eq!(
            (second.id, second.data),
            (p::ids(63).vel, p::pack_velocity(-0.5))
        );
        assert_eq!(yfoc_disable(id, 257), -1);
        assert_eq!(unsafe { yfoc_info(id, 1, std::ptr::null_mut()) }, -1);
        assert!(peer.recv_timeout(Duration::ZERO).unwrap().is_none());
        let s = session(id).unwrap();
        s.access(|_| {
            assert_eq!(yfoc_close(id), -1);
            assert_eq!(yfoc_set_velocity(id, 1, 1.0), -1);
            assert_eq!(yfoc_cancel_group(id), 0);
            Ok(())
        })
        .unwrap();
        assert_eq!(yfoc_close(id), 0);
        assert_eq!(yfoc_set_velocity(id, 1, 1.0), -1);
    }
}
