use yfoc_motor_sdk::{can, Joint};

fn main() -> Result<(), String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [interface, channel, node] = args.as_slice() else {
        return Err("用法：inspect <pcan|socketcan|slcan> <通道> <节点号>".into());
    };
    let node = node.parse().map_err(|_| "节点号应为 1～63")?;
    let bus = can::open(interface, channel, 1_000_000).map_err(|e| e.to_string())?;
    let mut joint = Joint::new(bus, node)?;
    let device = joint.discover()?.ok_or("节点未应答设备查询")?;
    let status = joint.status()?;
    let feedback = joint.feedback()?;
    println!(
        "{} firmware {}.{} · node {} · {} · fault {} · {:.4} rad · {:.2} rad/s",
        device.board_name,
        device.fw_major,
        device.fw_minor,
        node,
        status.state_name,
        status.fault_name,
        feedback.pos,
        feedback.vel
    );
    Ok(())
}
