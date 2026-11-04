#!/bin/bash
# Windows smoke test of the thread-safe read path, from Linux: cross-compiles
# NindReadAtSmoke.cpp with the nind sources (MinGW-w64, static) and runs it under Wine.
# Needs x86_64-w64-mingw32-g++-posix (package mingw-w64) and wine64.
# The unit tests are POSIX-only; this is what exercises NindFile::readAt's Windows branch
# (ReadFile + OVERLAPPED on a handle of its own) before the wheels are built on Windows.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../../src/cpp
out=${TMPDIR:-/tmp}/nind-windows-smoke
mkdir -p "$out"
x86_64-w64-mingw32-g++-posix -std=c++14 -O2 -static -I"$src" -I"$src/NindBasics" -I"$src/NindIndex" \
    -I"$src/NindRetrolexicon" "$here/NindReadAtSmoke.cpp" \
    "$src/NindBasics/NindFile.cpp" "$src/NindBasics/NindReadBuffer.cpp" "$src/NindBasics/NindPadFile.cpp" \
    "$src/NindBasics/NindSignalCatcher.cpp" "$src/NindIndex/NindIndex.cpp" "$src/NindIndex/NindTermIndex.cpp" \
    -o "$out/NindReadAtSmoke.exe"
cd "$out"
rm -f winsmoke.nindtermindex
WINEDEBUG=-all wine64 "$out/NindReadAtSmoke.exe" winsmoke
