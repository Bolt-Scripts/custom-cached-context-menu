#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
x86_64-w64-mingw32-g++ -std=c++20 -static -static-libgcc -static-libstdc++ \
  -I tests tests/logic_test.cpp -o tests/logic_test.exe \
  -lole32 -lshlwapi -luuid -lcomctl32 -lshell32 -luser32 -lgdi32 -ladvapi32 -luxtheme
WINEDEBUG=-all wine tests/logic_test.exe
