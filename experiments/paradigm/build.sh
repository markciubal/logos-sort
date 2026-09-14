#!/bin/sh
# Build the paradigm experiments. Needs an AVX-512 CPU and Intel x86-simd-sort.
#   git clone --depth 1 https://github.com/intel/x86-simd-sort.git
#   sh build.sh /path/to/x86-simd-sort/src   (defaults to ../x86-simd-sort/src)
set -e
XSS="${1:-../x86-simd-sort/src}"
g++ -O3 -std=c++17 -march=native -I "$XSS" paradigm.cpp -o paradigm
echo "built ./paradigm  (run it: ./paradigm)"
