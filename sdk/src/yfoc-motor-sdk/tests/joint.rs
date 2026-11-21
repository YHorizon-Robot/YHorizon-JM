use std::time::Duration;
use yfoc_motor_sdk::can::{virtual_pair, CanBus, Frame};
use yfoc_motor_sdk::protocol::{self as p, ids, pack_mgmt, pack_position};
use yfoc_motor_sdk::{ControlMode, Joint};

#[test]
fn validates_node_and_targets_before_sending() {
    let (mut bus, mut peer) = virtual_pair();
    for id in [0, 64, 255] {
        assert!(Joint::new(&mut bus, id).is_err());
    }
    let mut joint = Joint::new(&mut bus, 63).unwrap();
    for (rad, speed) in [
        (f64::NAN, 0.1),
        (1e9, 0.1),
        (0.0, -2.0),
        (0.0, f64::INFINITY),
    ] {
        assert!(joint.set_position(rad, speed).is_err());
    }
    assert!(joint.set_velocity(f64::NAN).is_err());
    assert_eq!(peer.recv_timeout(Duration::ZERO).unwrap(), None);
}

#[test]
fn position_updates_are_individual_frames_and_stop_is_acknowledged() {
    let (bus, mut peer) = virtual_pair();
    let ids = ids(7);
    let mut joint = Joint::new(bus, 7).unwrap();
    let endpoint = std::thread::spawn(move || {
        let next =
            |bus: &mut dyn CanBus| bus.recv_timeout(Duration::from_secs(1)).unwrap().unwrap();
        let cmd = next(&mut peer);
        assert_eq!(cmd.id, ids.mgmt);
        assert_eq!(cmd.data[0], p::CMD_SET_MODE);
        assert_eq!(cmd.data[2], p::MODE_POSITION);
        peer.send(&Frame {
            id: ids.ack,
            data: pack_mgmt(p::CMD_SET_MODE, cmd.data[1], 0),
        })
        .unwrap();
        assert_eq!(next(&mut peer).data[0], p::CMD_GET_STATUS);
        let mut status = [0; 8];
        status[7] = p::MODE_POSITION << 5;
        peer.send(&Frame {
            id: ids.status,
            data: status,
        })
        .unwrap();
        for pos in [0.1, 0.2] {
            assert_eq!(
                next(&mut peer),
                Frame {
                    id: ids.pos,
                    data: pack_position(pos, 1.0)
                }
            );
            let mut feedback = [0; 8];
            feedback[..4].copy_from_slice(&((pos / p::POS_LSB).round() as i32).to_le_bytes());
            peer.send(&Frame {
                id: ids.fb,
                data: feedback,
            })
            .unwrap();
        }
        let first = next(&mut peer);
        let second = next(&mut peer);
        assert_eq!((first.id, second.id), (ids.mgmt, ids.mgmt));
        assert_eq!(
            (first.data[0], second.data[0]),
            (p::CMD_DISABLE, p::CMD_DISABLE)
        );
        assert_ne!(first.data[1], second.data[1]);
        for cmd in [first, second] {
            peer.send(&Frame {
                id: ids.ack,
                data: cmd.data,
            })
            .unwrap();
        }
    });
    joint.set_mode(ControlMode::Position).unwrap();
    for pos in [0.1, 0.2] {
        joint.set_position(pos, 1.0).unwrap();
        let sample = joint
            .recv_feedback(Duration::from_millis(500))
            .unwrap()
            .unwrap();
        assert!((sample.pos - pos).abs() < 1e-8);
    }
    joint.disable().unwrap();
    endpoint.join().unwrap();
}
