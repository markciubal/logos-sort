//! Edge inputs that broke the C engine on 2026-09-19/20 (an infinity at a sample point crashed the parallel sort; NaN-free
//! data holding subnormals came back unsorted). All three engines share the design behind both defects, a value-linear
//! bucket map whose scale is `classes / range`, so the same inputs are run here. No NaN: the order with a NaN present is
//! documented as unspecified. Reference: `sort_unstable_by(f64::total_cmp)`; outputs must agree value for value, and as
//! bit patterns wherever the values are not zeros (-0.0 and +0.0 compare equal and may come back in either order).

use logos_sort::{sort_f64, sort_f64_inplace};

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn f(&mut self) -> f64 {
        (self.next() >> 11) as f64 / 9007199254740992.0
    }
}

fn gen(n: usize, shape: usize, r: &mut Rng) -> Vec<f64> {
    (0..n)
        .map(|_| match shape {
            0 => r.f() * 2e9 - 1e9,
            1 => 1000.0 * (1.0 - r.f() * 0.999999).powf(-1.0 / 1.2),
            _ => (r.next() % 4096) as f64 / 64.0,
        })
        .collect()
}

fn mix(a: &mut [f64], which: usize, r: &mut Rng) {
    let n = a.len();
    match which {
        0 => {}
        1 => a[0] = f64::NEG_INFINITY,
        2 => a[0] = f64::INFINITY,
        3 => {
            a[0] = f64::NEG_INFINITY;
            a[n - 1] = f64::INFINITY;
        }
        4 => {
            for i in (0..n).step_by(3) {
                a[i] = (r.f() - 0.5) * 1e-310; // a third subnormal, inside normal data
            }
        }
        5 => {
            for v in a.iter_mut() {
                *v = (r.f() - 0.5) * 4e-311; // nothing but subnormals
            }
        }
        _ => {
            a[0] = -1.7e308; // finite, but max - min overflows
            a[1] = 1.7e308;
        }
    }
}

fn check(name: &str, got: &[f64], want: &[f64]) {
    assert_eq!(got.len(), want.len(), "{name}: length");
    for i in 0..got.len() {
        let same = got[i] == want[i] && (got[i] == 0.0 || got[i].to_bits() == want[i].to_bits());
        assert!(same, "{name}: position {i}: got {:e}, want {:e}", got[i], want[i]);
    }
}

#[test]
fn infinities_subnormals_and_overflowing_ranges() {
    let mut r = Rng(88172645463325252);
    for &n in &[1000usize, 70_001, 300_001] {
        for shape in 0..3 {
            for which in 0..7 {
                let mut base = gen(n, shape, &mut r);
                mix(&mut base, which, &mut r);
                let mut want = base.clone();
                want.sort_unstable_by(f64::total_cmp);
                for &threads in &[1usize, 4, 8] {
                    let mut a = base.clone();
                    sort_f64(&mut a, threads);
                    check(&format!("sort_f64 n={n} shape={shape} mix={which} threads={threads}"), &a, &want);
                }
                let mut a = base.clone();
                sort_f64_inplace(&mut a, 1);
                check(&format!("sort_f64_inplace n={n} shape={shape} mix={which}"), &a, &want);
            }
        }
    }
}
