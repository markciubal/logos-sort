#!/bin/bash
# One sort per shape through a `--features trace` build of the Rust engine: which route ran and where its time went.
#   bash parity/trace.sh [n] [threads] [shape filter | extra | all]
# (the C column of this binary is not meaningful to read here; a trace build is never for timing.)
set -eo pipefail
cd "$(dirname "$0")/.."
export PATH="$HOME/.cargo/bin:$PATH" CARGO_TARGET_DIR="$HOME/logos-rs-target-trace"
B="$HOME/logos-rs-build"; mkdir -p "$B"
cargo build --release --lib --features trace
if [ ! -f "$B/logos_psort.o" ] || [ ../c/logos_psort.c -nt "$B/logos_psort.o" ]; then
    cc -O3 -std=gnu11 -pthread -fPIC -c ../c/logos_psort.c -o "$B/logos_psort.o"
fi
g++ -O2 -std=gnu++17 -pthread parity/parity.cpp "$B/logos_psort.o" "$CARGO_TARGET_DIR/release/liblogos_sort.a" -o "$B/parity-trace" -lm -ldl
PARITY_ONCE=1 exec "$B/parity-trace" "${1:-8388608}" 2 "${2:-1}" "${3:-}"
