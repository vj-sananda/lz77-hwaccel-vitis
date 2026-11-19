#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
rtl=$root/build/hls_cfg/hls/syn/verilog
test -f "$rtl/zstd_accel.v"
# Generated #0 initializers need no event scheduling in this cycle-driven test.
verilator --no-timing --cc --exe --build -j 4 -Wno-fatal --top-module zstd_accel \
    --Mdir "$root/build/verilator" -CFLAGS '-O2' -LDFLAGS '-lzstd' \
    "$rtl"/*.v "$root/tests/rtl_roundtrip.cpp" > reports/verilator_build.log 2>&1
"$root/build/verilator/Vzstd_accel" | tee reports/rtl_roundtrip.log
