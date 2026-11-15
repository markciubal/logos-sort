#!/bin/bash
# Builds and runs experiments/qpubench.cpp against the C engine:  bash experiments/qpubench.sh [file] [K] [rounds]
set -eo pipefail
cd "$(dirname "$0")"
B="$HOME/logos-qpu-build"; mkdir -p "$B"
cc -O3 -std=gnu11 -pthread -fPIC -c ../engines/c/logos_psort.c -o "$B/logos_psort.o"
g++ -O2 -std=gnu++17 -pthread qpubench.cpp "$B/logos_psort.o" -o "$B/qpubench" -lm
exec "$B/qpubench" "${1:-qpu-bank.bin}" "${2:-400}" "${3:-400}"
