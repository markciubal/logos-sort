//! The fair fight: logos (1 thread) vs sort_unstable, and logos (P threads) vs
//! rayon::par_sort_unstable, on identical data across the same shapes as the C
//! harness. Run: cargo run --release --example bench
use rayon::prelude::*;
use std::time::Instant;

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 { self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17; self.0 }
    fn f64(&mut self) -> f64 { (self.next() >> 11) as f64 / (1u64 << 53) as f64 }
}

fn gen(shape: &str, n: usize) -> Vec<f64> {
    let mut r = Rng(7);
    match shape {
        "uniform"      => (0..n).map(|_| r.f64() * 2e9 - 1e9).collect(),
        "near_sorted"  => { let mut a: Vec<f64> = (0..n).map(|i| i as f64).collect();
                            for _ in 0..n / 100 { let x = (r.next() % n as u64) as usize; let y = (r.next() % n as u64) as usize; a.swap(x, y); } a }
        "few_distinct" => (0..n).map(|_| (r.next() % 5) as f64 * 1e6).collect(),
        "heavy_tail"   => (0..n).map(|_| 1000.0 * (1.0 - r.f64() * 0.999999).powf(-1.0 / 1.2)).collect(),
        "bimodal"      => (0..n).map(|_| if r.next() & 1 == 1 { 1e9 } else { -1e9 } + r.f64() * 10.0).collect(),
        "quantized"    => (0..n).map(|_| (r.next() % 4096) as f64 / 64.0).collect(),
        "int_wide"     => (0..n).map(|_| (r.next() as i32) as f64).collect(),
        _ => unreachable!(),
    }
}

fn best(base: &[f64], reps: usize, mut f: impl FnMut(&mut Vec<f64>)) -> (f64, Vec<f64>) {
    let mut bestms = f64::INFINITY; let mut out = Vec::new();
    for _ in 0..reps {
        let mut a = base.to_vec();
        let t = Instant::now(); f(&mut a); let ms = t.elapsed().as_secs_f64() * 1e3;
        if ms < bestms { bestms = ms; out = a; }
    }
    (bestms, out)
}

fn main() {
    let p = std::thread::available_parallelism().map(|c| c.get()).unwrap_or(1);
    rayon::ThreadPoolBuilder::new().num_threads(p).build_global().unwrap();
    const N: usize = 8_000_000;
    println!("logos-sort (Rust, v2 interpolation leaf) — fair fight, n = 8M, best of 3, {p} threads\n");
    println!("{:<13} {:>12} {:>12} {:>7}  |  {:>12} {:>12} {:>7}", "shape", "sort_unst 1T", "logos 1T", "x", "rayon par PT", "logos PT", "x");
    let shapes = ["uniform", "near_sorted", "few_distinct", "heavy_tail", "bimodal", "quantized", "int_wide"];
    let mut bad = 0;
    for shape in shapes {
        let base = gen(shape, N);
        let mut reference = base.clone(); reference.sort_unstable_by(f64::total_cmp);
        let (t_std, _)  = best(&base, 3, |a| a.sort_unstable_by(f64::total_cmp));
        let (t_l1, o1)  = best(&base, 3, |a| logos_sort::sort_f64(a, 1));
        let (t_ray, _)  = best(&base, 3, |a| a.par_sort_unstable_by(f64::total_cmp));
        let (t_lp, op)  = best(&base, 3, |a| logos_sort::sort_f64(a, p));
        let ok1 = o1 == reference; let okp = op == reference;
        if !ok1 || !okp { bad += 1; }
        println!("{:<13} {:>10.1}ms {:>10.1}ms {:>6.2}x  |  {:>10.1}ms {:>10.1}ms {:>6.2}x{}",
                 shape, t_std, t_l1, t_std / t_l1, t_ray, t_lp, t_ray / t_lp,
                 if ok1 && okp { "" } else { "  <-- MISMATCH vs reference" });
    }
    // int32
    let basei: Vec<i32> = { let mut r = Rng(9); (0..N).map(|_| r.next() as i32).collect() };
    let mut refi = basei.clone(); refi.sort_unstable();
    let besti = |reps: usize, f: &mut dyn FnMut(&mut Vec<i32>)| -> (f64, Vec<i32>) {
        let mut b = f64::INFINITY; let mut out = Vec::new();
        for _ in 0..reps { let mut a = basei.clone(); let t = Instant::now(); f(&mut a); let ms = t.elapsed().as_secs_f64() * 1e3; if ms < b { b = ms; out = a; } }
        (b, out)
    };
    let (ts, _)  = besti(3, &mut |a| a.sort_unstable());
    let (t1, o1) = besti(3, &mut |a| logos_sort::sort_i32(a, 1));
    let (tr, _)  = besti(3, &mut |a| a.par_sort_unstable());
    let (tp, op) = besti(3, &mut |a| logos_sort::sort_i32(a, p));
    if o1 != refi || op != refi { bad += 1; }
    println!("{:<13} {:>10.1}ms {:>10.1}ms {:>6.2}x  |  {:>10.1}ms {:>10.1}ms {:>6.2}x", "int32 uniform", ts, t1, ts / t1, tr, tp, tr / tp);
    println!("\ncolumns: left = single-threaded fight (stdlib sort_unstable vs logos 1 thread);");
    println!("         right = PARALLEL fight (rayon par_sort_unstable vs logos, both on {p} threads). x > 1 means logos faster.");
    if p == 1 { println!("note: 1 CPU here — the right-hand columns show overhead only; run on a multi-core box."); }
    println!("\nRESULT: {}", if bad == 0 { "all outputs match the reference sort" } else { "OUTPUT MISMATCHES — do not trust timings" });
    std::process::exit(if bad == 0 { 0 } else { 1 });
}
