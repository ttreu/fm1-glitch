#!/bin/sh
# Build and run the native GLITCH DSP tests with the host compiler.
set -e
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/glitch-tests"; mkdir -p "$out"
cc="clang -std=c11 -O2 -Wall -Wextra -DFM1_HOST_TEST -Iapp -Isdk/include"
$cc tests/glitch_dsp_test.c app/fx_echo.c app/fx_reverb.c -o "$out/dsp" -lm
"$out/dsp"
