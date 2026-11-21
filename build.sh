#!/usr/bin/env bash
# Cross-compile Breaker.exe from Linux/macOS with Zig (pip install ziglang, or a zig binary on PATH).
set -euo pipefail
cd "$(dirname "$0")"
if [[ -z "${ZIG:-}" ]]; then
  if command -v zig >/dev/null 2>&1; then ZIG=zig; else ZIG="python3 -m ziglang"; fi
fi
mkdir -p dist
$ZIG c++ -target x86_64-windows-gnu -std=c++17 -O2 -s -municode \
  -DUNICODE -D_UNICODE ${BREAKER_DEFS:-} \
  -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers \
  src/*.cpp res/breaker.rc -o dist/Breaker.exe \
  -Wl,--subsystem,windows \
  -ld2d1 -ldwrite -lole32 -loleaut32 -luuid -ldwmapi -lshcore -lshell32 -luser32 -lgdi32 -ladvapi32 -lshlwapi \
  -lcomctl32 -luxtheme -lwinhttp -lbcrypt -lcomdlg32 -lversion -lpowrprof -lwinspool -liphlpapi -lwlanapi \
  -lrasapi32 -loleacc -lwtsapi32
rm -f dist/Breaker.pdb
ls -la dist/Breaker.exe
