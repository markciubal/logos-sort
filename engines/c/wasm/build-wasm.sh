#!/bin/sh
# build-wasm.sh — compile logos_psort.c to threaded WebAssembly with Emscripten.
#
# NOTE: this was NOT run in the environment it shipped from (no toolchain there).
# It is the standard Emscripten recipe; verify on your machine.
#
# Prereqs (once):
#   git clone https://github.com/emscripten-core/emsdk
#   cd emsdk && ./emsdk install latest && ./emsdk activate latest
#   source ./emsdk_env.sh          # puts emcc on PATH
set -e

SRC=../logos_psort.c

# Shared flags. -pthread is what turns pthread_create into Web Workers that share
# one WebAssembly.Memory (a SharedArrayBuffer). Export the C entry points plus
# malloc/free so JS can stage data into the wasm heap.
COMMON="-O3 -pthread \
  -sEXPORTED_FUNCTIONS=_logos_sort_f64,_logos_sort_i32,_logos_num_cpus,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPF64,HEAP32 \
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB \
  -sMODULARIZE=1 -sEXPORT_NAME=createLogosSort"

# --- Node build (easiest to test; Node may block on join off the main loop) ---
emcc $SRC $COMMON \
  -sENVIRONMENT=node \
  -sPTHREAD_POOL_SIZE=8 \
  -o logos_psort.node.mjs

# --- Web build (ES module for browser + workers) ---
# Pool sized to the machine's core count at runtime. Extra threads beyond the
# pool are still created dynamically, but a right-sized pool avoids that cost.
emcc $SRC $COMMON \
  -sENVIRONMENT=web,worker -sEXPORT_ES6=1 \
  -sPTHREAD_POOL_SIZE='navigator.hardwareConcurrency' \
  -o logos_psort.web.mjs

echo "built logos_psort.node.mjs and logos_psort.web.mjs (+ .wasm, +.worker.js)"
