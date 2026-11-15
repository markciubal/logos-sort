#!/bin/bash
# One file of the C port, compiled alone:  bash parity/portcheck.sh <name> [test [filter]]     (see PORTING.md)
# <name> = base | leaf | knot | part | count | reloc | ord1 | ord2 | disp | all
set -eo pipefail
cd "$(dirname "$0")/.."
name="$1"; [ -n "$name" ] || { echo "usage: portcheck.sh <name> [test [filter]]"; exit 2; }
export PATH="$HOME/.cargo/bin:$PATH" CARGO_TARGET_DIR="$HOME/logos-rs-port-$name"
if [ "$2" = test ]; then
    cargo test --release --lib --features "port_$name" -- "port::${3:-}"
else
    cargo check --lib --features "port_$name"
fi
