# Porting the C engine to Rust: the rules

`src/port/` is a **transliteration** of `engines/c/logos_psort.c` (the f64 sort path, default build) into Rust. The goal
is the SAME ALGORITHM: the same routes in the same order, the same gates, the same constants, the same arithmetic, so that
every input takes the same path in both engines. It is not a redesign and not an idiomatic rewrite. When the C does
something odd, the Rust does the same odd thing; improvements are out of scope and are recorded as notes, not made.

The port is split into files by C line range. Several people port different ranges at the same time, so names and types
must come out identical without coordination. That is what these rules are for: follow them mechanically.

## What is live

The default build defines exactly these: `LOGOS_SODO`, `LOGOS_HISTFREE`, `LOGOS_AVX2_LEAF`, `LOGOS_SIMD_MERGE`,
`LOGOS_HAVE_SSE2`, `LOGOS_HAVE_CUTS`, `LOGOS_HAVE_DOMINANT`. Every other `LOGOS_*` macro is undefined (so `#ifndef
LOGOS_NO_X` blocks are live, `#ifdef LOGOS_TRACE` / `LOGOS_PROFILE` / `LOGOS_XSS_LEAF` / `LOGOS_EXMAP` blocks are not).
`__x86_64__`, `__linux__`, `__GNUC__` are defined. Port only live code.

Also dropped, on purpose:

- **`PROF(...)`, `PATH_TAG`** and everything profile-only: omit.
- **`getenv`**: every environment switch reads as UNSET. Take the branch the code takes when `getenv` returns NULL, and
  where a value is read with a default, use the default. A `g_*` flag that only ever holds an env switch's value becomes
  a `const` with its unset value (keep the name), so the code that reads it stays literal.
- **Allocation failure**: Rust aborts on it. Port the success path; drop `if (!p) { ...fallback... }` branches and the
  `qsort` fallbacks behind them. Keep a decline that is a real decision (a gate), not an out-of-memory path.
- **Thread-creation failure** handling (`made < P` orphan paths): the pool here always runs every worker; port the
  normal path.
- **Opt-in paths that an env switch alone enables** (the in-place partition `ipp_*`, the linked-list knot placement
  `list_*` / `knot_place_list`, the pool trim, `LOGOS_MC_CONCAT`, `radix_f64_full` behind `g_quant_radix`): omit, and
  say so in your report.

**Keep `TRACE(...)` lines**, with the same text (`TRACE!("par: nearsorted declined")`, C format specs turned into Rust
ones). A trace build of each engine is how route equivalence is checked. Keep the route witnesses too
(`logos_twist_last` counters, `g_reach_last`, `g_mirror_count`, ...): they are how tests name the path taken.

## Names and types (mechanical)

| C | Rust |
|---|---|
| function `foo_bar` | `pub(crate) unsafe fn foo_bar` (same name, same parameter order and names) |
| `static inline` / `noinline` | `#[inline]` / `#[inline(never)]` (keep `noinline`: the C has it for stack-frame reasons) |
| `double *` / `const double *` | `*mut f64` / `*const f64` (raw pointers everywhere the C has pointers; no slices in signatures) |
| `size_t`, `int`, `unsigned`, `long` | `usize`, `i32`, `u32`, `i64` |
| `uint8_t` ... `uint64_t`, `int16_t` ... | `u8` ... `u64`, `i16` ... |
| an `int` used as a boolean (returns, flags, struct fields) | stays `i32` holding 0 / 1 (so every signature is predictable) |
| `void *` | `*mut c_void` (`core::ffi::c_void`, re-exported by the prelude) |
| struct `foo_t` | `#[repr(C)] #[derive(Clone, Copy)] pub(crate) struct foo_t` (same name, same field names, fields `pub(crate)`) |
| `memset(&x, 0, sizeof x)` | `x = core::mem::zeroed()` |
| `#define NAME value` | `pub(crate) const NAME: <type> = value;` (same name; `usize` for sizes and counts, `f64` for ratios, `i32` for small ints compared with `int`s) |
| function-like macro | `#[inline(always)] pub(crate) unsafe fn` with the macro's name lowercased, or a `macro_rules!` when it must be textual |
| file-scope `static T g_x` | `pub(crate) static mut g_x: T` read and written BY VALUE only (`core::ptr::addr_of_mut!` when a pointer is needed) |
| `__thread` / `_Thread_local` global | `thread_local! { pub(crate) static g_x: Cell<T> }` |
| `_Atomic` / `atomic_*` | `AtomicUsize` / `AtomicI32` / ... with the same memory orders (`atomic_load` etc. without an order are `SeqCst`) |
| `malloc` / `calloc` / `free` / `realloc`, `memcpy` / `memmove` / `memset` | the prelude's functions of the same names (see below). `big_alloc` is in `base.rs` |
| worker `void *fn(void *arg)` | `pub(crate) unsafe fn fn(arg: *mut c_void) -> *mut c_void` |
| AVX2 / SSE2 intrinsics | `core::arch::x86_64::*` (re-exported by the prelude), same intrinsic names |
| a function inside `#pragma GCC target("avx2,fma")` (or with the target attribute) | `#[target_feature(enable = "avx2,fma")] pub(crate) unsafe fn` |
| `__builtin_prefetch(p, 1, 0)` | `_mm_prefetch::<_MM_HINT_NTA>(p as *const i8)` (write intent has no stable equivalent) |
| `__builtin_expect`, `__builtin_ctzll`, `__builtin_popcount` | drop the hint; `.trailing_zeros()`, `.count_ones()` |
| `isfinite(x)`, `sqrt`, `floor`, `fma(a, b, c)` | `x.is_finite()`, `x.sqrt()`, `x.floor()`, `a.mul_add(b, c)` |

Arithmetic must match C exactly: unsigned wraparound is `wrapping_*`, a C cast is an `as` cast (note that Rust's float to
integer `as` SATURATES and maps NaN to 0 where C is undefined: keep the C guard in front of the cast, do not rely on
either behaviour), integer division and shifts as written, `double` expressions in the same order of operations. Compare
doubles with the same operator the C uses (`<`, `<=`, `!(a < b)`): NaN behaviour depends on it.

Lints are allowed crate-wide for the port (`non_camel_case_types`, `non_snake_case`, `non_upper_case_globals`,
`dead_code`, `static_mut_refs`, `unused_*`, clippy). Do not fight the names.

## The prelude: `src/port/mod.rs`

Always compiled, and authoritative. Do not redefine anything in it:

- `malloc(bytes) -> *mut u8`, `calloc(count, size) -> *mut u8`, `realloc(p, bytes) -> *mut u8`, `free(p)` (any pointer
  type, null is fine; blocks are 64-byte aligned and never null), `memcpy(dst, src, bytes)`, `memmove`, `memset(p, byte,
  bytes)` (generic over pointer types; byte counts as in C).
- `TRACE!(...)`, `TRACE_START!()` (no-ops without the `trace` feature).
- The constants of C lines 116-291, `weyl_pos`, `SAMPLE_POS`, `effective_threads`, `boost_threads`, `logos_simd_kind`.
- The shared structs: `ctx_t`, `ctx_fit_t`, `rdesc_t`, `sodo_t`, `psamp_t`, `pscan_t`, `scratch_t`.

## Files, and compiling yours alone

| file | feature | C lines |
|---|---|---|
| `base.rs` | `port_base` | 496-850 except the AVX2 kernel, 3160-3353, 4806-4829, 5573-5700: small sorts, int32 radix, contexts, the worker pool and `par_*`, the buffer pool, scratch, `sample_sort` |
| `leaf.rs` | `port_leaf` | 1367-1986, 2739-3159: the kernel's scan / fused first partition (the kernel itself is `crate::kernel`), flash, coarse, run merges, `sort_fn`, `leaf_f64` |
| `knot.rs` | `port_knot` | 1981-2738: run plans, the knot sweep, the two-strand and k-way merges, placement, the half placement |
| `part.rs` | `port_part` | 3354-4277, 4473-4640, 8683-8793: splitter tables, bucket maps, the histogram path, the histogram-free partition, heavy values, two halves |
| `count.rs` | `port_count` | 4641-5572, 7433-7619, 7850-7940: few values, hashed counts, lattices, dense integers, run lengths, a dominant value |
| `reloc.rs` | `port_reloc` | 5866-7034: run detection, strays, kinks, relocation and the knot route's parallel form, `relocate_try_f64` |
| `ord1.rs` | `port_ord1` | 5700-5865, 7035-7432, 7620-7849: near-sorted, short reach, the shared sample (`sodo_*`), tail and head merges |
| `ord2.rs` | `port_ord2` | 7941-8682: streams, rotations, the sample by position, cuts, the wave, mirror, seam, loop-back, the prescan |
| `disp.rs` | `port_disp` | 8794-9458, 10104-10370: the dispatcher, the thin front, the entry |

The ranges are approximate at their edges: a function belongs to the file whose range holds its DEFINITION. If something
live near your edge is in nobody's range, port it and say so.

Every file starts with `use super::*;`. Everything in it is `pub(crate)`; `mod.rs` glob re-exports every module, so a
function is called by its bare C name from anywhere.

**Calling something outside your range that the prelude does not have:** write the call exactly as the C does, and put a
stub with the C signature (transliterated by the table above) in a module at the BOTTOM of your file:

```rust
#[cfg(not(feature = "port_all"))]
mod missing {
    use super::*;
    pub(crate) unsafe fn leaf_f64(a: *mut f64, n: usize, buf: *mut f64, c: *mut ctx_t) -> *mut f64 { unimplemented!() }
    pub(crate) const RELOC_MIN_N: usize = 0;          // a constant defined elsewhere: any value
}
#[cfg(not(feature = "port_all"))]
use missing::*;
```

A struct defined outside your range goes there too, with the C's fields. At integration `port_all` is on, the stubs
vanish and the real definitions resolve by name, which is why the signatures must be the mechanical ones.

Check your file alone, from PowerShell (never Git Bash; the toolchain is in WSL):

```
wsl -e bash /mnt/c/Users/markc/Coding/logos-sort-repo/logos-sort/engines/rust/parity/portcheck.sh <name>         # cargo check
wsl -e bash /mnt/c/Users/markc/Coding/logos-sort-repo/logos-sort/engines/rust/parity/portcheck.sh <name> test    # cargo test, your module's tests
```

`<name>` is the feature without `port_` (`base`, `leaf`, ...). Each name has its own build directory, so checks do not
collide. Your file must pass `cargo check` with no errors. Add `#[cfg(test)] mod tests` for whatever in your range can
be tested alone (a sort or a merge must give an ascending permutation of its input's bit patterns; include NaN-free
data with duplicates, signed zeros and infinities), and run them.

Edit ONLY your own file. Do not touch `mod.rs`, `Cargo.toml`, other port files, or anything outside `src/port/`.

## What to report

1. Every function and struct you ported (name only), and every live one in your range you did NOT, with the reason.
2. Everything in your `missing` module (the cross-file calls), with the signature you assumed.
3. Anything where a literal port was impossible and what you did instead.
4. Anything in the C that looked like a defect. Do not fix it in the port; report it.
5. `cargo check` and test results, verbatim last lines.
