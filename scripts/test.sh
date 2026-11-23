#!/usr/bin/env bash
# Builds whisper.cpp if needed, then runs the C++ core unit tests.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ -f build/whisper/install/lib/libwhisper.a ]] || scripts/build_whisper.sh
swift build --product TTSCoreTests
"$(swift build --show-bin-path)/TTSCoreTests" "$@"
