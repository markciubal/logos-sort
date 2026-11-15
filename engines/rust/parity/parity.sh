#!/bin/bash
# Builds the Rust engine (staticlib) and the C engine, links both into one bench and runs it.
#   bash parity/parity.sh [n] [pairs] [threads] [shape filter | extra | all]     (from engines/rust, inside WSL)
#   bash parity/parity.sh build                                                   builds only
# Build products go to $HOME/logos-rs-build and $HOME/logos-rs-target, never into the tree.
set -eo pipefail
cd "$(dirname "$0")/.."
export PATH="$HOME/.cargo/bin:$PATH" CARGO_TARGET_DIR="$HOME/logos-rs-target"
B="$HOME/logos-rs-build"; mkdir -p "$B"
cargo build --release --lib
if [ ! -f "$B/logos_psort.o" ] || [ ../c/logos_psort.c -nt "$B/logos_psort.o" ]; then
    cc -O3 -std=gnu11 -pthread -fPIC -c ../c/logos_psort.c -o "$B/logos_psort.o"
fi
g++ -O2 -std=gnu++17 -pthread parity/parity.cpp "$B/logos_psort.o" "$CARGO_TARGET_DIR/release/liblogos_sort.a" -o "$B/parity" -lm -ldl
if [ "$1" = "build" ]; then exit 0; fi
exec "$B/parity" "$@"
