#!/bin/bash
# The two AVX2 kernels alone (see kernbench.cpp):  bash parity/kernbench.sh [N] [pairs]
set -eo pipefail
cd "$(dirname "$0")/.."
export PATH="$HOME/.cargo/bin:$PATH" CARGO_TARGET_DIR="$HOME/logos-rs-target"
B="$HOME/logos-rs-build"; mkdir -p "$B"
cargo build --release --lib
cc -O3 -std=gnu11 -pthread -fPIC -w -fvisibility=hidden -c parity/kernwrap.c -o "$B/kernwrap.o"
objcopy --keep-global-symbol=ck_kernel "$B/kernwrap.o"
g++ -O2 -std=gnu++17 -pthread parity/kernbench.cpp "$B/kernwrap.o" "$CARGO_TARGET_DIR/release/liblogos_sort.a" -o "$B/kernbench" -lm -ldl
exec "$B/kernbench" "$@"
