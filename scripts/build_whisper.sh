#!/usr/bin/env bash
# Builds the vendored whisper.cpp + ggml as static libraries (Metal, embedded
# shader library) into build/whisper/install. SwiftPM links against them; see
# Package.swift. Safe to re-run: CMake only rebuilds what changed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/whisper.cpp"
BUILD="$ROOT/build/whisper/build"
INSTALL="$ROOT/build/whisper/install"
CMAKE="${CMAKE:-$(command -v cmake || echo /opt/homebrew/bin/cmake)}"
JOBS="$(sysctl -n hw.ncpu)"

"$CMAKE" -S "$SRC" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_INSTALL_PREFIX="$INSTALL" \
  -DBUILD_SHARED_LIBS=OFF \
  -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=OFF -DWHISPER_BUILD_SERVER=OFF \
  -DWHISPER_ALL_WARNINGS=OFF \
  -DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON \
  -DGGML_BLAS=ON -DGGML_OPENMP=OFF \
  -DGGML_NATIVE=OFF -DGGML_CPU_ARM_ARCH=armv8.4-a+dotprod+fp16 \
  -Wno-dev -Wno-deprecated >/dev/null 2>&1 || { echo "cmake configure failed"; exit 1; }
"$CMAKE" --build "$BUILD" -j "$JOBS" >/dev/null
"$CMAKE" --install "$BUILD" >/dev/null
echo "whisper.cpp static libs: $INSTALL/lib"
ls "$INSTALL/lib"/*.a
