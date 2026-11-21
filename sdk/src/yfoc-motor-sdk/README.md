# YHorizon-JM 关节电机 Rust SDK

通过经典 CAN 访问 YHorizon-JM 关节电机。SDK 直接打开适配器，可在自己的控制程序中使用。Windows 支持 PCAN，Linux 支持 SocketCAN，也可使用 SLCAN。固件 1.6 支持本文列出的单轴及组同步功能。

## 接入

保留同级的 `yfoc-can`、`yfoc-protocol` 和 `yfoc-motor-sdk` 三个 crate，在应用的 `Cargo.toml` 中添加本地依赖。以下路径从应用目录指向 SDK，按实际目录调整。

```toml
[dependencies]
yfoc-motor-sdk = { path = "../sdk/src/yfoc-motor-sdk" }
```

在含这三个 crate 的 Cargo 工作区执行只读示例，可检查板型、固件版本、故障、位置和速度。

```text
cargo run -p yfoc-motor-sdk --example inspect -- pcan PCAN_USBBUS1 1
cargo run -p yfoc-motor-sdk --example inspect -- socketcan can0 1
```

PCAN 需要系统的 PCAN-Basic 驱动。SocketCAN 的接口速率由操作系统配置。组同步使用 1 Mbit/s，同一通道应由一个程序负责收发。

## 单轴更新位置

先探测设备，设置一次位置模式，随后每次调用 `set_position` 就提交一帧新目标。输出轴目标单位为 rad，速度上限单位为 rad/s。发送间隔由调用方调度。普通位置模式会继续保持最后目标，结束时用 `disable` 关桥。

```no_run
use yfoc_motor_sdk::{can, ControlMode, Joint};

# fn main() -> Result<(), String> {
let bus = can::open("pcan", "PCAN_USBBUS1", 1_000_000).map_err(|e| e.to_string())?;
let mut joint = Joint::new(bus, 1)?;
let device = joint.discover()?.ok_or("节点未应答")?;
if !device.control_ext { return Err("需要支持读取反馈和 DISABLE 的固件".into()); }
let start = joint.feedback()?.pos;
joint.set_mode(ControlMode::Position)?;
let result = (|| {
    joint.set_position(start, 0.5)?;
    // 运动控制循环可据此接收该节点的普通反馈，再提交下一目标。
    let sample = joint.recv_feedback(std::time::Duration::from_millis(20))?
        .ok_or("位置反馈超时")?;
    println!("输出轴实测 {:.4} rad", sample.pos);
    Ok::<(), String>(())
})();
let stopped = joint.disable();
result.and(stopped)
# }
```

`feedback()` 主动查询，`recv_feedback(timeout)` 读取节点自发回复的普通运动反馈。普通反馈没有周期号，固件每次处理接收队列时可合并同轴实时指令。需要速度模式时，使用 `set_mode(ControlMode::Velocity)` 后调用 `set_velocity`。电流环的实测和配置分别由 `current_state()`、`current_config()` 读取。单节点客户端等待应答时会消费总线上的其他帧，组同步期间请把接收权完整交给 `multi_axis::execute`。

## 多轴同步与在线更新

固件 1.6 在 START 前接收每个关节的一至两个绝对目标，支持 1～16 轴。运行中每 5 ms 广播会话和周期号，各轴在本地 4 kHz 外环插补，并每 5 ms 回传一次位置和速度。主机按周期交付全组反馈。运行中不能用新的逐轴目标替换预装轨迹，逐个调用单轴接口也没有共同的生效时刻。

```no_run
use std::sync::atomic::AtomicBool;
use yfoc_motor_sdk::{can::CanBus, multi_axis::{self, Axis, Event, Plan}};

fn move_pair(bus: &mut dyn CanBus, targets_rad: [f64; 2], cancel: &AtomicBool)
    -> Result<String, String>
{
    let axes = [1, 7].into_iter().zip(targets_rad).map(|(node, target)|
        Axis { node, target, second: None }).collect();
    let plan = Plan { axes, seconds: 1.0 };
    multi_axis::execute(bus, &plan, cancel, |event| {
        if let Event::Cycle(samples) = event {
            // 每个样本含节点号、共同周期号、参考位置、实测位置和速度。
            // 这里可将 samples 交给记录线程，避免阻塞 CAN 调度。
            let _ = samples;
        }
    })
}
```

另一线程可将 `cancel` 置为 true 以取消。正常结束后 SDK 发送组停止并逐轴确认已退出组运动。固件丢失同步约 30 ms 自行关桥。启用真实机械负载前应考虑停止后转矩释放。
