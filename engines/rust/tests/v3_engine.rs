//! The v3 engine against the standard library: every shape x size x thread count comes back
//! ascending under `<` and as the same multiset of BIT PATTERNS as its input (so a signed zero, an
//! infinity or a NaN payload cannot be invented or lost). Rows that are built for one route say
//! which route must have taken them; a test that cannot tell which path ran tests nothing in
//! particular.

use logos_sort::{last_route, sort_f64, sort_f64_v3, Route};

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

const SHAPES: &[&str] = &[
    "uniform", "heavy_tail", "bimodal", "clustered", "clustered_32", "int_wide", "int_narrow", "distinct_2", "few_distinct", "distinct_256",
    "distinct_65536", "quantized", "prices", "near_sorted", "sorted", "reversed", "all_equal", "block_perm", "block_overlap", "block_twist",
    "stairs", "logs8", "sawtooth", "prefix_95", "kinks", "latency", "zipf", "signed_zeros", "infinities", "subnormals", "lognormal", "few_plus_rare",
    "lattice_spill", "tail_strays", "blocks_1024", "hundred_values", "clean_kinks", "local_shuffle",
];

fn gen(shape: &str, n: usize, seed: u64) -> Vec<f64> {
    let mut r = Rng(seed | 1);
    let mut a = vec![0.0f64; n];
    let blocks = |a: &mut Vec<f64>, r: &mut Rng, kind: u32| {
        let nb = 64usize.min(n.max(1));
        let blk = (n + nb - 1) / nb;
        let mut perm: Vec<usize> = (0..nb).collect();
        for k in (1..nb).rev() {
            let j = (r.next() % (k as u64 + 1)) as usize;
            perm.swap(k, j);
        }
        for i in 0..n {
            let b = perm[(i / blk).min(nb - 1)];
            let mut o = i % blk;
            let mut base = b as f64 * blk as f64;
            if kind == 1 && (b & 7) == 1 { base -= blk as f64 * 0.1; }
            if kind == 2 && (b & 1) == 1 { o = blk - 1 - o; }
            a[i] = base + o as f64 + if kind == 2 { 0.25 } else { r.f() * 0.5 };
        }
    };
    match shape {
        "uniform" => a.iter_mut().for_each(|v| *v = r.f() * 2e9 - 1e9),
        "heavy_tail" => a.iter_mut().for_each(|v| *v = 1000.0 * (1.0 - r.f() * 0.999999).powf(-1.0 / 1.2)),
        "bimodal" => a.iter_mut().for_each(|v| *v = if r.next() & 1 == 1 { 1e9 } else { -1e9 } + r.f() * 10.0),
        "clustered" => a.iter_mut().for_each(|v| *v = (r.next() % 16) as f64 * 1e8 + r.f() * 1e5),
        "clustered_32" => a.iter_mut().for_each(|v| *v = (r.next() % 32) as f64 * 1e8 + r.f() * 1e5),
        "int_wide" => a.iter_mut().for_each(|v| *v = (r.next() as i32) as f64),
        "int_narrow" => a.iter_mut().for_each(|v| *v = (r.next() % 1_000_000) as f64),
        "distinct_2" => a.iter_mut().for_each(|v| *v = (r.next() % 2) as f64 * 1e6 + 0.5),
        "few_distinct" => a.iter_mut().for_each(|v| *v = (r.next() % 5) as f64 * 1e6),
        "distinct_256" => a.iter_mut().for_each(|v| *v = (r.next() % 256) as f64 * 1000.0 + 0.5),
        "distinct_65536" => a.iter_mut().for_each(|v| *v = (r.next() % 65536) as f64 * 1000.0 + 0.5),
        "quantized" => a.iter_mut().for_each(|v| *v = (r.next() % 4096) as f64 / 64.0),
        "prices" => a.iter_mut().for_each(|v| *v = (r.next() % 20_000) as f64 / 100.0),
        "near_sorted" => {
            for i in 0..n { a[i] = i as f64 + 0.5; }
            for _ in 0..n / 100 {
                let (i, j) = ((r.next() % n as u64) as usize, (r.next() % n as u64) as usize);
                a.swap(i, j);
            }
        }
        "sorted" => for i in 0..n { a[i] = i as f64 * 1.5 },
        "reversed" => for i in 0..n { a[i] = (n - i) as f64 * 1.5 },
        "all_equal" => a.iter_mut().for_each(|v| *v = 42.5),
        "hundred_values" => {
            // a hundred values on no lattice
            let t: Vec<f64> = (0..100).map(|_| r.f() * 1e6 - 5e5).collect();
            a.iter_mut().for_each(|v| *v = t[(r.next() % 100) as usize]);
        }
        "block_perm" => blocks(&mut a, &mut r, 0),
        "block_overlap" => blocks(&mut a, &mut r, 1),
        "block_twist" => blocks(&mut a, &mut r, 2),
        "blocks_1024" => {
            // many small sorted blocks, shuffled: half of the boundaries ascend
            let nb = (n / 2048).max(1);
            let mut perm: Vec<usize> = (0..nb).collect();
            for k in (1..nb).rev() {
                let j = (r.next() % (k as u64 + 1)) as usize;
                perm.swap(k, j);
            }
            let blk = (n + nb - 1) / nb;
            for i in 0..n { a[i] = perm[(i / blk).min(nb - 1)] as f64 * blk as f64 + (i % blk) as f64 + r.f() * 0.5; }
        }
        "stairs" => {
            let blk = (n + 63) / 64;
            for i in 0..n { a[i] = (i / blk) as f64 * blk as f64 * 0.8 + (i % blk) as f64 + r.f() * 0.5; }
        }
        "logs8" => {
            let blk = (n + 7) / 8;
            for i in 0..n { a[i] = (i % blk) as f64 * 8.0 + (i / blk) as f64 + r.f() * 0.5; }
        }
        "sawtooth" => for i in 0..n { a[i] = (i % 4096) as f64 + r.f() * 0.5 },
        "prefix_95" => {
            let k = n - n / 20;
            for i in 0..k { a[i] = i as f64 + 0.5; }
            for i in k..n { a[i] = r.f() * n as f64; }
        }
        "kinks" => {
            for i in 0..n { a[i] = i as f64 + 0.5; }
            if n > 1 {
                for _ in 0..n / 100 {
                    let i = (r.next() % (n as u64 - 1)) as usize;
                    a.swap(i, i + 1);
                }
            }
        }
        "latency" => for i in 0..n { a[i] = i as f64 + r.f() * 200.0 },
        // one pair in fifty swapped, no two swaps touching
        "clean_kinks" => {
            for i in 0..n { a[i] = i as f64 + 0.5; }
            let mut i = 3;
            while i + 1 < n {
                a.swap(i, i + 1);
                i += 20 + (r.next() % 60) as usize;
            }
        }
        // every element within two places of where it belongs
        "local_shuffle" => {
            for i in 0..n { a[i] = i as f64 + 0.5; }
            let mut i = 0;
            while i + 3 <= n {
                let j = i + (r.next() % 3) as usize;
                a.swap(i, j);
                i += 3;
            }
        }
        "zipf" => a.iter_mut().for_each(|v| *v = (1.0 / (r.f() + 1e-6)).floor()),
        "signed_zeros" => a.iter_mut().for_each(|v| {
            *v = match r.next() % 4 {
                0 => -0.0,
                1 => 0.0,
                _ => r.f() - 0.5,
            }
        }),
        "infinities" => a.iter_mut().for_each(|v| {
            *v = match r.next() % 64 {
                0 => f64::INFINITY,
                1 => f64::NEG_INFINITY,
                _ => r.f() * 1e300,
            }
        }),
        "subnormals" => a.iter_mut().for_each(|v| *v = (r.next() % 1_000_003) as f64 * 4.9e-324),
        "lognormal" => a.iter_mut().for_each(|v| {
            let (u, w) = (r.f(), r.f());
            *v = (2.0 * (-2.0 * (u + 1e-300).ln()).sqrt() * (6.283185307179586 * w).cos()).exp()
        }),
        // five common values and, one time in ten thousand, something else
        "few_plus_rare" => a.iter_mut().for_each(|v| *v = if r.next() % 10_000 == 0 { r.f() * 1e7 } else { (r.next() % 5) as f64 * 1e6 }),
        // a lattice with a few values in a hundred thousand off it, or far outside its window
        "lattice_spill" => a.iter_mut().for_each(|v| {
            *v = match r.next() % 100_000 {
                0 => (r.next() % 4096) as f64 / 64.0 + 0.001,
                1 => 1e15 * r.f(),
                _ => (r.next() % 4096) as f64 / 64.0,
            }
        }),
        // one ascending strand with strays that stand above it and fall below it, some side by side
        "tail_strays" => {
            for i in 0..n { a[i] = i as f64 + 0.5; }
            let mut i = 7;
            while i + 3 < n {
                match r.next() % 4 {
                    0 => a[i] = -1.0 - r.f(),
                    1 => a[i] = 2.0 * n as f64 + r.f(),
                    2 => { a[i] = 3.0 * n as f64; a[i + 1] = -5.0; }
                    _ => { a[i] = -7.0; a[i + 1] = -8.0; a[i + 2] = 4.0 * n as f64; }
                }
                i += 40 + (r.next() % 200) as usize;
            }
        }
        _ => panic!("unknown shape {shape}"),
    }
    a
}

fn bits_sorted(a: &[f64]) -> Vec<u64> {
    let mut b: Vec<u64> = a.iter().map(|v| v.to_bits()).collect();
    b.sort_unstable();
    b
}
/// Ascending under `<` (skipped when the input holds a NaN: the order is then unspecified) and
/// the same bit patterns as the input.
fn check(input: &[f64], got: &[f64], what: &str) {
    assert_eq!(input.len(), got.len(), "{what}: length");
    if !input.iter().any(|v| v.is_nan()) {
        for i in 1..got.len() {
            assert!(!(got[i] < got[i - 1]), "{what}: descends at {i}: {} then {}", got[i - 1], got[i]);
        }
    }
    assert!(bits_sorted(input) == bits_sorted(got), "{what}: not a permutation of the input's bit patterns");
}

#[test]
fn every_shape_size_and_thread_count() {
    let sizes = [2usize, 3, 4, 5, 8, 15, 16, 17, 31, 33, 63, 64, 65, 100, 511, 512, 513, 2047, 2048, 2049, 4097, 10_000, 65_537, 100_003, 250_000];
    for &shape in SHAPES {
        for &n in &sizes {
            let input = gen(shape, n, 0x9E37 + n as u64);
            for t in [1usize, 2, 3, 4, 8, 14] {
                if t > 1 && n < 100_000 && t != 4 { continue; }   /* below one thread per 100,000 a parallel request runs sequentially */
                let mut got = input.clone();
                sort_f64(&mut got, t);
                check(&input, &got, &format!("{shape} n={n} threads={t}"));
            }
        }
    }
}

#[test]
fn large_arrays_all_threads() {
    for &shape in SHAPES {
        for &n in &[1_000_003usize, 2_100_000] {
            let input = gen(shape, n, 77 + n as u64);
            for t in [1usize, 2, 3, 6, 14] {
                let mut got = input.clone();
                sort_f64(&mut got, t);
                check(&input, &got, &format!("{shape} n={n} threads={t}"));
            }
        }
    }
}

#[test]
fn routes_are_the_ones_the_shapes_were_built_for() {
    let rows: &[(&str, &[Route])] = &[
        ("sorted", &[Route::Sorted]),
        ("reversed", &[Route::Reversed]),
        ("all_equal", &[Route::Sorted]),
        ("distinct_2", &[Route::FewValues]),
        ("few_distinct", &[Route::FewValues]),
        ("distinct_256", &[Route::Lattice]),
        ("quantized", &[Route::Lattice]),
        ("lattice_spill", &[Route::Lattice]),
        ("near_sorted", &[Route::OneStrand, Route::Runs]),   /* in parallel each chunk finds its own piece of the strand */
        ("clean_kinks", &[Route::Sorted]),                    /* repaired by the prescan */
        ("tail_strays", &[Route::OneStrand, Route::Runs]),   /* in parallel each chunk finds its own piece of the strand */
        ("block_perm", &[Route::Runs]),
        ("block_overlap", &[Route::Runs]),
        ("stairs", &[Route::Runs]),
        ("block_twist", &[Route::Runs]),
        ("blocks_1024", &[Route::Runs]),
        ("latency", &[Route::Reach]),
        ("prefix_95", &[Route::OneStrand, Route::Runs]),      /* the prefix is the strand, the unsorted tail its strays */
        ("uniform", &[Route::Kernel, Route::Partition]),
        ("heavy_tail", &[Route::Kernel, Route::Partition]),
        ("bimodal", &[Route::Kernel, Route::Partition]),
    ];
    for &(shape, want) in rows {
        for t in [1usize, 4, 14] {
            let n = 2_100_000;
            let input = gen(shape, n, 5);
            let mut got = input.clone();
            sort_f64_v3(&mut got, t);
            let r = last_route();
            check(&input, &got, &format!("{shape} threads={t}"));
            let expect = if want.len() == 2 { want[(t > 1) as usize] } else { want[0] };
            assert_eq!(r, expect, "{shape} at {t} thread(s) took {r:?}");
        }
    }
    // one thread only: the integer radix, the hashed counts, and a lattice of 65,536 values in 2.1M
    // elements (32 a slot: enough at one thread, short of the 40 a slot that four threads ask for)
    for (shape, want) in [("int_wide", Route::IntRadix), ("few_plus_rare", Route::Lattice), ("hundred_values", Route::HashedCounts), ("distinct_65536", Route::Lattice)] {
        let input = gen(shape, 2_100_000, 5);
        let mut got = input.clone();
        sort_f64_v3(&mut got, 1);
        let r = last_route();
        check(&input, &got, shape);
        assert_eq!(r, want, "{shape} at one thread took {r:?}");
    }
}

#[test]
fn nan_is_a_permutation_and_returns() {
    for &shape in SHAPES {
        for &n in &[3usize, 64, 65, 600, 5_000, 300_000, 1_200_000] {
            for (k, at) in [0usize, n / 3, n - 1].into_iter().enumerate() {
                let mut input = gen(shape, n, 31 + k as u64);
                input[at] = f64::NAN;
                if n > 100 {
                    input[n / 2] = f64::from_bits(0x7ff8_0000_dead_beef);   /* a payload, which must survive */
                    input[n / 7] = -f64::NAN;
                }
                for t in [1usize, 4, 14] {
                    let mut got = input.clone();
                    sort_f64(&mut got, t);
                    check(&input, &got, &format!("NaN in {shape} n={n} at {at} threads={t}"));
                }
            }
        }
    }
}

#[test]
fn signed_zeros_keep_their_count() {
    for &n in &[10usize, 64, 100, 5_000, 70_000, 300_001, 1_100_000] {
        for t in [1usize, 4, 14] {
            let mut r = Rng(n as u64 * 3 + 1);
            // zeros of both signs among other values, and zeros of both signs alone
            for mix in 0..3 {
                let input: Vec<f64> = (0..n)
                    .map(|_| match (mix, r.next() % 5) {
                        (_, 0) => -0.0,
                        (_, 1) => 0.0,
                        (0, _) => r.f() - 0.5,
                        (1, _) => (r.next() % 3) as f64 - 1.0,
                        _ => if r.next() & 1 == 0 { -0.0 } else { 0.0 },
                    })
                    .collect();
                let mut got = input.clone();
                sort_f64(&mut got, t);
                check(&input, &got, &format!("signed zeros mix={mix} n={n} threads={t}"));
            }
        }
    }
}

#[test]
fn concurrent_callers() {
    // the C engine allows one call at a time per process; this one must not care
    let handles: Vec<_> = (0..6)
        .map(|k| {
            std::thread::spawn(move || {
                for round in 0..6 {
                    let shape = SHAPES[(k * 5 + round * 3) % SHAPES.len()];
                    let n = 150_000 + 90_000 * ((k + round) % 5);
                    let input = gen(shape, n, (k * 100 + round) as u64);
                    let mut got = input.clone();
                    sort_f64(&mut got, 1 + (k + round) % 6);
                    check(&input, &got, &format!("concurrent {shape} n={n} caller={k}"));
                }
            })
        })
        .collect();
    for h in handles { h.join().unwrap(); }
}

#[test]
fn release_and_sort_again() {
    for round in 0..3 {
        for shape in ["uniform", "block_perm", "quantized", "int_wide"] {
            let input = gen(shape, 400_000, round);
            for t in [1usize, 4] {
                let mut got = input.clone();
                sort_f64(&mut got, t);
                check(&input, &got, &format!("after release: {shape} threads={t}"));
            }
        }
        logos_sort::release_buffers();
    }
}
