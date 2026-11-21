use super::*;

#[test]
fn commissioning_bandwidths_match_validated_boards() {
    assert_eq!(current_bandwidths(1), &[180.0, 260.0, 350.0, 425.0]);
    for board in [2, 3] {
        assert_eq!(current_bandwidths(board), &[180.0, 260.0, 350.0]);
    }
}

#[test]
fn current_pi_score_requires_every_round_and_uses_the_median() {
    let mut c = CurrentCandidate {
        bandwidth: 425.0,
        kp: 2.7,
        ki: 8200.0,
        scores: vec![4.0, 0.1],
    };
    assert_eq!(c.median_score(3), None);
    c.scores.push(3.6);
    assert_eq!(c.median_score(3), Some(3.6));
}

#[test]
fn saving_rl_invalidates_tuning_and_cogging_on_readback_and_export() {
    let mut before = [0; WORDS];
    before[0] = HAS_ENC | HAS_AUX | HAS_RL | HAS_CURRENT | HAS_PID | HAS_COG;
    before[1..6].copy_from_slice(&[3.0, 0.0009, 0.0011, 2.2, 6700.0].map(f32::to_bits));
    before[6] = u32::from_le_bytes([14, 255, 1, 0]);
    before[7] = 0.4f32.to_bits();
    before[8..13].copy_from_slice(&[0.01, 0.1, 30.0, 0.0, 0.0].map(f32::to_bits));
    before[TABLE] = 10;
    let mut after = before;
    set_motor_rl(&mut after, 3.1, 0.001, 0.0012);
    assert_eq!(after[0], HAS_ENC | HAS_AUX | HAS_RL);
    assert_eq!(&after[4..], &before[4..]);

    let (mut host, mut device) = yfoc_can::virtual_pair();
    let responder = thread::spawn(move || {
        let mut saved = before;
        let mut staged = saved;
        loop {
            let frame = device
                .recv_timeout(Duration::from_secs(5))
                .unwrap()
                .unwrap();
            let [op, seq, index, _, a, b, c, d] = frame.data;
            let mut reply = [0; 8];
            reply[..2].copy_from_slice(&[op, seq]);
            match (op, index) {
                (INFO, 0) => reply[2..].copy_from_slice(&[1, 1, 7, 14, 30, 31]),
                (INFO, 1) => reply[2] = 14,
                (INFO, 2) => reply[6] = saved[0] as u8,
                (INFO, 3) => (),
                (PROFILE, 0) => staged = saved,
                (PROFILE, 1) => {
                    let bytes: Vec<_> = staged.iter().flat_map(|v| v.to_le_bytes()).collect();
                    assert_eq!(u32::from_le_bytes([a, b, c, d]), crc32fast::hash(&bytes));
                    saved = staged;
                }
                (WRITE, i) => staged[usize::from(i)] = u32::from_le_bytes([a, b, c, d]),
                (READ, i) => reply[2..6].copy_from_slice(&staged[usize::from(i)].to_le_bytes()),
                _ => panic!("unexpected request {op:#x}, index {index}"),
            }
            device
                .send(&Frame {
                    id: 0x481,
                    data: reply,
                })
                .unwrap();
            if (op, index) == (INFO, 3) {
                return saved;
            }
        }
    });
    let cancel = AtomicBool::new(false);
    let run = |host: &mut dyn CanBus, action| execute(host, action, &cancel, |_| {}).unwrap();
    let initial = run(
        &mut host,
        Action::Run {
            node: 1,
            step: Step::Inspect,
        },
    );
    assert_eq!(initial.flags, before[0]);
    let candidate = Candidate {
        node: 1,
        board: 1,
        step: Step::Rl,
        before,
        after,
    };
    assert_eq!(run(&mut host, Action::Save(candidate)).flags, after[0]);

    let path = std::env::temp_dir().join(format!("yfoc-rl-export-{}.json", std::process::id()));
    run(
        &mut host,
        Action::Export {
            node: 1,
            path: path.clone(),
        },
    );
    let contents = std::fs::read(&path).unwrap();
    std::fs::remove_file(path).unwrap();
    let json: Value = serde_json::from_slice(&contents).unwrap();
    assert_eq!(responder.join().unwrap(), after);
    assert_eq!(json["stored_flags"], after[0]);
    assert_eq!(json["encoder"]["pole_pairs"], 14);
    assert_eq!(json["motor_rl"]["phase_resistance_ohm"], 3.1f32 as f64);
    for field in ["current_pi", "velocity_pi", "position_pid"] {
        assert_eq!(json[field]["autotuned"], false);
    }
    assert_eq!(json["cogging"]["enabled"], false);
    assert_eq!(json["cogging"]["iq_ma"], Value::Null);
}
