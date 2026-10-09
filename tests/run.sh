#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
x86_64-w64-mingw32-g++ -std=c++20 -static -static-libgcc -static-libstdc++ \
  -I tests tests/logic_test.cpp -o tests/logic_test.exe \
  -lole32 -lshlwapi -luuid -lcomctl32 -lshell32 -luser32 -lgdi32 -ladvapi32 -luxtheme -lversion -ld3d11 -ld2d1 -ldwrite -ldcomp
WINEDEBUG=-all wine tests/logic_test.exe

# Production build check: no CMO_TESTING, so the test hooks are excluded.
x86_64-w64-mingw32-g++ -std=c++20 -static -static-libgcc -static-libstdc++ \
  -I tests tests/production_compile.cpp -o tests/production_compile.exe \
  -lole32 -lshlwapi -luuid -lcomctl32 -lshell32 -luser32 -lgdi32 -ladvapi32 -luxtheme -lversion -ld3d11 -ld2d1 -ldwrite -ldcomp
echo "production compile OK"
