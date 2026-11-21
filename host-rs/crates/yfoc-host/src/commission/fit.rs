use std::f64::consts::TAU;

use super::{Info, Sample, POINTS};

fn signed(x: u16) -> f64 {
    f64::from(x as i16) * 0.001
}
fn wrap(x: f64) -> f64 {
    (x + TAU / 2.0).rem_euclid(TAU) - TAU / 2.0
}

fn unwrap(values: impl Iterator<Item = f64>) -> Vec<f64> {
    let mut prev = None;
    values
        .map(|x| {
            let next = prev.map_or(x, |p| p + wrap(x - p));
            prev = Some(next);
            next
        })
        .collect()
}

fn mean(data: &[f64]) -> f64 {
    data.iter().sum::<f64>() / data.len() as f64
}

fn line(x: &[f64], y: &[f64]) -> Option<(f64, f64, f64)> {
    let mx = mean(x);
    let my = mean(y);
    let xx: f64 = x.iter().map(|v| (v - mx).powi(2)).sum();
    let xy: f64 = x.iter().zip(y).map(|(a, b)| (a - mx) * (b - my)).sum();
    let slope = xy / xx;
    if !slope.is_finite() {
        return None;
    }
    let offset = my - slope * mx;
    let err = (x
        .iter()
        .zip(y)
        .map(|(a, b)| (a * slope + offset - b).powi(2))
        .sum::<f64>()
        / x.len() as f64)
        .sqrt();
    Some((slope, offset, err))
}

fn circular_mean(values: &[f64]) -> (f64, f64) {
    let (sin, cos) = values
        .iter()
        .fold((0.0, 0.0), |(s, c), a| (s + a.sin(), c + a.cos()));
    (
        sin.atan2(cos).rem_euclid(TAU),
        sin.hypot(cos) / values.len() as f64,
    )
}

pub(super) struct Alignment {
    pub poles: u8,
    pub direction: i8,
    pub offset: f64,
    pub p95: f64,
    pub aux: Option<i8>,
}

pub(super) fn encoder(info: &Info, samples: [&[Sample]; 2]) -> Result<Alignment, String> {
    let mut tracks = Vec::with_capacity(2);
    for sweep in samples {
        let raw = unwrap(sweep.iter().map(|x| f64::from(x[0]) * TAU / info.cpr));
        let elec = unwrap(sweep.iter().map(|x| f64::from(x[2]) * TAU / 65536.0));
        if raw.len() < 200 || (raw[raw.len() - 1] - raw[0]).abs() < TAU * 0.91 {
            return Err("扫转覆盖不足一圈，检查是否失步或激励不足".into());
        }
        let (slope, _, _) = line(&raw, &elec).ok_or("极对数拟合无效")?;
        tracks.push((slope, raw, elec));
    }
    let slope = tracks[0].0;
    let rounded = slope.round();
    if !(2.0..=30.0).contains(&rounded.abs())
        || (slope - rounded).abs() > 0.22
        || (tracks[1].0 - rounded).abs() > 0.22
    {
        return Err(format!(
            "极对数拟合正反不一致（{slope:.3} / {:.3}，板卡参考 {}）",
            tracks[1].0, info.pole_hint
        ));
    }
    let residuals: Vec<Vec<f64>> = tracks
        .iter()
        .map(|(_, raw, elec)| {
            raw.iter()
                .zip(elec)
                .map(|(r, e)| wrap(e - rounded * r))
                .collect()
        })
        .collect();
    let (a, ca) = circular_mean(&residuals[0]);
    let (b, cb) = circular_mean(&residuals[1]);
    let (offset, _) = circular_mean(&[a, b]);
    let mut joint: Vec<_> = residuals
        .iter()
        .flatten()
        .map(|r| wrap(r - offset).abs())
        .collect();
    joint.sort_by(f64::total_cmp);
    let p95 = joint[joint.len() * 95 / 100];
    if ca.min(cb) < 0.86 || wrap(a - b).abs() > 1.15 || p95 > 1.05 {
        return Err(format!(
            "电角度残差过大，P95 {p95:.3} rad，正反偏差 {:.3} rad",
            wrap(a - b).abs()
        ));
    }
    let aux = if info.dual {
        let n1 = f64::from(info.teeth[0]);
        let n2 = f64::from(info.teeth[1]);
        let mut bins = [0u16; 128];
        for s in samples.into_iter().flatten() {
            let r = (n1 * f64::from(s[0]) + n2 * info.aux_dir * f64::from(s[1])) / info.cpr;
            let bin = (r.rem_euclid(1.0) * 128.0) as usize % 128;
            bins[bin] = bins[bin].saturating_add(1);
        }
        let mut best = (0, 0);
        let mut empty = 0;
        for i in 0..256 {
            empty = if bins[i % 128] < 2 { empty + 1 } else { 0 };
            if empty > best.0 {
                best = (empty, i);
            }
        }
        let width = (128.0 - f64::from(best.0)) / 256.0;
        if best.0 >= 128 || width > info.aux_tol {
            return Err(format!(
                "双编码器游标残差 {:.3} 圈号，超出 {:.3} 限制",
                width, info.aux_tol
            ));
        }
        let center = ((best.1 + 1) % 128) as f64 / 128.0 + width;
        Some(
            ((center - (center + 0.5).floor()) * 256.0)
                .round()
                .clamp(-128.0, 127.0) as i8,
        )
    } else {
        None
    };
    Ok(Alignment {
        poles: rounded.abs() as u8,
        direction: rounded.signum() as i8,
        offset,
        p95,
        aux,
    })
}

pub(super) fn resistance(samples: &[Sample]) -> Result<f64, String> {
    let mut i = Vec::new();
    let mut v = Vec::new();
    for step in 0..4 {
        let window: Vec<_> = samples
            .iter()
            .filter(|s| {
                let ms = u32::from(s[2]) / 20;
                (step * 200 + 85..step * 200 + 190).contains(&ms)
            })
            .collect();
        if window.len() < 50 {
            return Err("稳态电阻采样不完整".into());
        }
        i.push(window.iter().map(|s| signed(s[0])).sum::<f64>() / window.len() as f64);
        v.push(window.iter().map(|s| signed(s[1])).sum::<f64>() / window.len() as f64);
    }
    let (r, _, rms) = line(&i, &v).ok_or("电阻拟合无效")?;
    let span = i.iter().copied().fold(f64::NEG_INFINITY, f64::max)
        - i.iter().copied().fold(f64::INFINITY, f64::min);
    if span < 0.08 || rms > 0.18 || !(0.03..=40.0).contains(&r) {
        return Err(format!(
            "相电阻拟合不可靠（{r:.3} Ω，电流跨度 {span:.3} A，残差 {rms:.3} V）"
        ));
    }
    Ok(r)
}

fn inductance_window(
    samples: &[Sample],
    r: f64,
    period: usize,
    lo: usize,
    hi: usize,
) -> Option<f64> {
    let mut i_sum = vec![0.0; period];
    let mut v_sum = vec![0.0; period];
    let mut count = vec![0; period];
    for s in samples {
        let tick = usize::from(s[2]);
        if !(lo..hi).contains(&tick) {
            continue;
        }
        let k = tick % period;
        i_sum[k] += signed(s[0]);
        v_sum[k] += signed(s[1]);
        count[k] += 1;
    }
    if count.iter().any(|n| *n < 8) {
        return None;
    }
    let i: Vec<_> = i_sum
        .iter()
        .zip(&count)
        .map(|(x, n)| x / f64::from(*n))
        .collect();
    let v: Vec<_> = v_sum
        .iter()
        .zip(&count)
        .map(|(x, n)| x / f64::from(*n))
        .collect();
    // Use the amplitude of the fundamental. Differentiating individual ADC
    // samples magnifies switching noise, and a fixed sample delay changes phase
    // but not the magnitude of the impedance.
    let harmonic = |data: &[f64]| {
        let (real, imag) = data
            .iter()
            .enumerate()
            .fold((0.0, 0.0), |(re, im), (k, x)| {
                let angle = TAU * k as f64 / period as f64;
                (re + x * angle.cos(), im + x * angle.sin())
            });
        2.0 * real.hypot(imag) / period as f64
    };
    let current = harmonic(&i);
    let voltage = harmonic(&v);
    if current < 0.035 || voltage < 0.1 {
        return None;
    }
    let impedance = voltage / current;
    let reactance_squared = impedance.powi(2) - r.powi(2);
    if reactance_squared <= 0.0 {
        return None;
    }
    let omega = 2.0 * (std::f64::consts::PI / period as f64).tan() / 0.00005;
    Some(reactance_squared.sqrt() / omega)
}

pub(super) fn inductance(samples: &[Sample], r: f64) -> Result<f64, String> {
    let a = inductance_window(samples, r, 16, 130, 586).ok_or("短周期电感响应信噪比不足")?;
    let b = inductance_window(samples, r, 28, 655, 1190).ok_or("长周期电感响应信噪比不足")?;
    let result = (a + b) * 0.5;
    if (a - b).abs() > result * 0.40 || !(0.000005..=0.1).contains(&result) {
        return Err(format!(
            "两组脉冲的电感结果不一致（{:.3} / {:.3} mH）",
            a * 1e3,
            b * 1e3
        ));
    }
    Ok(result)
}

pub(super) fn current_score(samples: &[Sample]) -> Option<f64> {
    if samples.len() < 700 {
        return None;
    }
    let target = signed(samples.last()?[2]);
    if target.abs() < 0.12 {
        return None;
    }
    let a: Vec<_> = samples
        .chunks_exact(20)
        .map(|w| w.iter().map(|s| signed(s[0])).sum::<f64>() / (20.0 * target))
        .collect();
    let step = a
        .iter()
        .zip(samples.chunks_exact(20))
        .position(|(_, s)| signed(s[19][2]).abs() >= target.abs() * 0.9)?;
    let a = &a[step..];
    if a.len() < 24 {
        return None;
    }
    let tail = &a[a.len() - 14..];
    let steady = mean(tail);
    let oscillation =
        (tail.iter().map(|x| (x - steady).powi(2)).sum::<f64>() / tail.len() as f64).sqrt();
    let peak = a.iter().copied().fold(0.0, f64::max);
    if !(0.82..=1.18).contains(&steady) || peak > 1.42 || oscillation > 0.19 {
        return None;
    }
    Some(
        a.iter().take(20).map(|x| (1.0 - x).abs()).sum::<f64>()
            + 4.0 * oscillation
            + 6.0 * (peak - 1.0).max(0.0),
    )
}

pub(super) fn motion_score(samples: &[Sample], goal: i16, position: bool) -> Option<f64> {
    if samples.len() < 60 {
        return None;
    }
    let sig: Vec<_> = samples
        .iter()
        .map(|x| (x[usize::from(!position)] as i16) as f64 / f64::from(goal))
        .collect();
    let tail = &sig[sig.len() * 75 / 100..];
    let steady = mean(tail);
    let peak = sig.iter().copied().fold(0.0, f64::max);
    if (steady - 1.0).abs() > if position { 0.22 } else { 0.20 } || peak > 1.50 {
        return None;
    }
    let ripple =
        (tail.iter().map(|x| (x - steady).powi(2)).sum::<f64>() / tail.len() as f64).sqrt();
    if ripple > 0.23 {
        return None;
    }
    let peak_i = samples
        .iter()
        .map(|s| signed(s[2]).abs())
        .fold(0.0, f64::max);
    Some(
        sig.iter().map(|x| (1.0 - x).abs()).sum::<f64>() / sig.len() as f64
            + (steady - 1.0).abs()
            + 2.0 * ripple
            + 2.0 * (peak - 1.0).max(0.0)
            + 0.05 * peak_i,
    )
}

fn steady_samples(data: &[Sample], velocity: i16) -> Result<&[Sample], String> {
    let at = data.get(128..).ok_or("低速扫描样本不足")?;
    let average = at.iter().map(|x| signed(x[1])).sum::<f64>() / at.len() as f64;
    let target = f64::from(velocity) * 0.001;
    let stalled = at.iter().filter(|x| signed(x[1]) / target < 0.28).count();
    if (average / target - 1.0).abs() > 0.20 || stalled > at.len() / 16 {
        return Err(format!(
            "低速扫描未稳定，目标 {target:.3} rad/s，实测 {average:.3} rad/s"
        ));
    }
    Ok(at)
}

pub(super) fn cogging(
    info: &Info,
    data: [&[Sample]; 2],
    goal: i16,
) -> Result<[i16; POINTS], String> {
    let mut values = [[0.0; POINTS]; 2];
    let mut repeats = [[0; POINTS]; 2];
    for (n, run) in data.iter().enumerate() {
        let run = steady_samples(run, if n == 0 { goal } else { -goal })?;
        let path = unwrap(run.iter().map(|s| f64::from(s[0]) * TAU / info.cpr));
        if (path[path.len() - 1] - path[0]).abs() < 2.0 * TAU {
            return Err("齿槽扫描未覆盖至少两整圈".into());
        }
        for s in run {
            let k = ((f64::from(s[0]) / info.cpr) * POINTS as f64) as usize % POINTS;
            values[n][k] += signed(s[2]);
            repeats[n][k] += 1;
        }
    }
    let mut both = [0.0; POINTS];
    let mut covered = [false; POINTS];
    for i in 0..POINTS {
        if repeats[0][i] > 0 && repeats[1][i] > 0 {
            both[i] = 0.5
                * (values[0][i] / f64::from(repeats[0][i])
                    + values[1][i] / f64::from(repeats[1][i]));
            covered[i] = true;
        }
    }
    if covered.iter().filter(|x| **x).count() < POINTS * 7 / 8 {
        return Err("双向扫描的共同角度覆盖率不足".into());
    }
    for i in 0..POINTS {
        if covered[i] {
            continue;
        }
        let past = (1..=8).find(|j| covered[(i + POINTS - j) % POINTS]);
        let next = (1..=8).find(|j| covered[(i + j) % POINTS]);
        let (p, n) = past.zip(next).ok_or("补偿表存在连续无效区间")?;
        both[i] = (both[(i + POINTS - p) % POINTS] * n as f64 + both[(i + n) % POINTS] * p as f64)
            / (p + n) as f64;
    }
    let dc = mean(&both);
    Ok(std::array::from_fn(|i| {
        let filtered =
            0.25 * both[(i + POINTS - 1) % POINTS] + 0.5 * both[i] + 0.25 * both[(i + 1) % POINTS];
        (850.0 * (filtered - dc)).round().clamp(-400.0, 400.0) as i16
    }))
}

pub(super) fn ripple(a: &[Sample], b: &[Sample], goal: i16) -> Result<(f64, f64), String> {
    let mut err = 0.0;
    let mut peak = 0.0f64;
    let mut count = 0;
    for (s, g) in [(a, goal), (b, -goal)] {
        let s = steady_samples(s, g)?;
        let v = f64::from(g) * 0.001;
        for sample in s {
            err += (signed(sample[1]) - v).powi(2);
            peak = peak.max(signed(sample[2]).abs());
            count += 1;
        }
    }
    Ok(((err / f64::from(count)).sqrt(), peak))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn electrical_wrap_and_sign() {
        let info = Info {
            board: 3,
            calibrated: false,
            dual: false,
            current_loop: true,
            pole_hint: 14,
            cpr: 16384.0,
            gear: 1.0,
            teeth: [0; 2],
            aux_dir: 0.0,
            aux_tol: 0.0,
            kt: 0.0,
            vbus: 24.0,
            stored_flags: 0,
            abs_unknown: false,
        };
        let make = |sign: f64| {
            (0..780)
                .map(|i| {
                    let a = 0.7 + sign * (i as f64) * 0.0085;
                    let e = -14.0 * a + 1.1 + sign * 0.04;
                    [
                        (a.rem_euclid(TAU) * info.cpr / TAU) as u16,
                        0,
                        (e.rem_euclid(TAU) * 65536.0 / TAU) as u16,
                    ]
                })
                .collect::<Vec<_>>()
        };
        let a = make(1.0);
        let b = make(-1.0);
        let fit = encoder(&info, [&a, &b]).unwrap();
        assert_eq!((fit.poles, fit.direction), (14, -1));
        assert!((fit.offset - 1.1).abs() < 0.01);
    }

    #[test]
    fn fits_resistance_and_inductance() {
        let rs = 5.2;
        let l = 0.0024;
        let r_data: Vec<_> = (1..1600)
            .map(|k| {
                let t = k * 10;
                let v = [1.5, 3.0, 4.5, 2.5][t / 4000];
                [
                    ((v - 0.14) / rs * 1000.0) as u16,
                    (v * 1000.0) as u16,
                    t as u16,
                ]
            })
            .collect();
        assert!((resistance(&r_data).unwrap() - rs).abs() < 0.03);
        let mut i = 0.0f64;
        let l_data: Vec<_> = (1..1200)
            .map(|k| {
                let period = if k < 600 { 16 } else { 28 };
                let v = if k % period < period / 2 { 1.7 } else { -1.7 };
                i = (i + 0.00005 / l * (v - rs * i / 2.0)) / (1.0 + 0.00005 * rs / (2.0 * l));
                [
                    (i * 1000.0).round() as i16 as u16,
                    (v * 1000.0) as i16 as u16,
                    k,
                ]
            })
            .collect();
        assert!((inductance(&l_data, rs).unwrap() - l).abs() < 0.00005);
    }

    #[test]
    fn cogging_uses_common_angle_component() {
        let info = Info {
            board: 1,
            calibrated: true,
            dual: true,
            current_loop: true,
            pole_hint: 14,
            cpr: 16384.0,
            gear: 8.0,
            teeth: [30, 31],
            aux_dir: 1.0,
            aux_tol: 0.4,
            kt: 0.23,
            vbus: 24.0,
            stored_flags: 0,
            abs_unknown: false,
        };
        let make = |dir: f64| {
            (0..1400)
                .map(|i| {
                    let a = 0.2 + dir * f64::from(i) * 0.005 * 0.4 * info.gear;
                    let iq = 0.13 * dir + 0.12 * (12.0 * a).sin();
                    [
                        (a.rem_euclid(TAU) * info.cpr / TAU) as u16,
                        (400.0 * dir) as i16 as u16,
                        (1000.0 * iq).round() as i16 as u16,
                    ]
                })
                .collect::<Vec<_>>()
        };
        let a = make(1.0);
        let b = make(-1.0);
        let table = cogging(&info, [&a, &b], 400).unwrap();
        assert!(table.iter().map(|v| i32::from(*v)).sum::<i32>().abs() <= POINTS as i32);
        assert!(*table.iter().max().unwrap() > 85);
        assert!(*table.iter().min().unwrap() < -85);
        assert!(cogging(&info, [&a[..200], &b[..200]], 400).is_err());
    }
}
