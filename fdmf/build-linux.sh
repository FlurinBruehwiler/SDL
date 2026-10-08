#!/usr/bin/env bash
# Builds the linux-x64 libSDL3.so that FDMF ships instead of the one PanGui.SDL3 bundles.
# Audio is off, FDMF only initializes video. Vulkan stays, PanGui renders through the SDL GPU API.
# Needs SDL's build dependencies, see docs/README-linux.md.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build-fdmf"

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_AUDIO=OFF -DSDL_PIPEWIRE=OFF
cmake --build "$build"

mkdir -p "$(dirname "$0")/linux-x64"
cp -L "$build/libSDL3.so.0" "$(dirname "$0")/linux-x64/libSDL3.so"
strip --strip-unneeded "$(dirname "$0")/linux-x64/libSDL3.so"
