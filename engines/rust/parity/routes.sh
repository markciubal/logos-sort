#!/bin/bash
# Route equivalence of the Rust port against the C engine (parity/routes.cpp):
#   bash parity/routes.sh [n list] [P list] [shape filter]        ROUTES_VERBOSE=1 prints every cell
#   bash parity/routes.sh build                                    builds only
set -eo pipefail
cd "$(dirname "$0")/.."
export PATH="$HOME/.cargo/bin:$PATH" CARGO_TARGET_DIR="$HOME/logos-rs-port-all"
B="$HOME/logos-rs-build"; mkdir -p "$B"
cargo build --release --lib --features port_all
if [ ! -f "$B/logos_psort.o" ] || [ ../c/logos_psort.c -nt "$B/logos_psort.o" ]; then
    cc -O3 -std=gnu11 -pthread -fPIC -c ../c/logos_psort.c -o "$B/logos_psort.o"
fi
g++ -O2 -std=gnu++17 -pthread parity/routes.cpp "$B/logos_psort.o" "$CARGO_TARGET_DIR/release/liblogos_sort.a" -o "$B/routes" -lm -ldl
if [ "$1" = "build" ]; then exit 0; fi
exec "$B/routes" "$@"
