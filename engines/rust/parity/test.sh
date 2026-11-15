#!/bin/bash
# cargo test --release inside WSL, with the build tree outside the repository:  bash parity/test.sh [test filter]
set -eo pipefail
cd "$(dirname "$0")/.."
export PATH="$HOME/.cargo/bin:$PATH" CARGO_TARGET_DIR="$HOME/logos-rs-target"
cargo test --release "$@"
