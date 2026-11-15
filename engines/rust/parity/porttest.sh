#!/bin/bash
# The port's unit tests with every file joined, one test at a time (the engine allows one sort at a time, as the C does):
#   bash parity/porttest.sh [filter]            PORT_TRACE=1: the trace build, output shown
set -o pipefail
cd "$(dirname "$0")/.."
export PATH="$HOME/.cargo/bin:$PATH" CARGO_TARGET_DIR="$HOME/logos-rs-port-all"
cargo test --release --lib --features "port_all${PORT_TRACE:+,trace}" -- "port::${1:-}" --test-threads=1 ${PORT_TRACE:+--nocapture} 2>&1 | grep -v "^warning" | tail -n "${PORT_TAIL:-90}"
