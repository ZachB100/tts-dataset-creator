#!/usr/bin/env bash
# Builds "TTS Dataset Creator.app" with SwiftPM only (no Xcode), then ad-hoc signs it.
#   scripts/bundle_app.sh            -> build/TTS Dataset Creator.app
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
APP_NAME="TTS Dataset Creator"
EXE="TTSDatasetCreator"
VERSION="$(sed -n 's/^public let appVersion = "\(.*\)"/\1/p' Sources/TTSKit/Paths.swift)"
OUT="$ROOT/build"
APP="$OUT/$APP_NAME.app"

# 1. whisper.cpp + ggml static libraries (CMake, Metal with embedded shaders)
if [[ ! -f build/whisper/install/lib/libwhisper.a || "${REBUILD_WHISPER:-0}" == "1" ]]; then
  scripts/build_whisper.sh
fi

# 2. Swift + C++ (release)
swift build -c release --product "$EXE"
swift build -c release --product ttsdc
BIN="$(swift build -c release --show-bin-path)"

# 3. assemble the bundle
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/Licenses"
cp "$BIN/$EXE" "$APP/Contents/MacOS/$EXE"
cp "$BIN/ttsdc" "$APP/Contents/MacOS/ttsdc"   # the CLI, for scripted runs
cp third_party/whisper.cpp/LICENSE "$APP/Contents/Resources/Licenses/whisper.cpp-LICENSE.txt"
cp LICENSE "$APP/Contents/Resources/Licenses/TTSDatasetCreator-LICENSE.txt" 2>/dev/null || true
printf 'nlohmann/json 3.11.2, MIT License, Copyright (c) 2013-2022 Niels Lohmann\n' \
  > "$APP/Contents/Resources/Licenses/nlohmann-json-NOTICE.txt"

ICONSET="$OUT/AppIcon.iconset"
if [[ ! -f "$OUT/AppIcon.icns" ]]; then
  swift scripts/make_icon.swift "$ICONSET" && iconutil -c icns "$ICONSET" -o "$OUT/AppIcon.icns" || echo "icon skipped"
  rm -rf "$ICONSET"
fi
[[ -f "$OUT/AppIcon.icns" ]] && cp "$OUT/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME</string>
  <key>CFBundleExecutable</key><string>$EXE</string>
  <key>CFBundleIdentifier</key><string>org.ttsdatasetcreator.app</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>LSArchitecturePriority</key><array><string>arm64</string></array>
  <key>LSApplicationCategoryType</key><string>public.app-category.productivity</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>CFBundleDocumentTypes</key>
  <array><dict>
    <key>CFBundleTypeName</key><string>Audio</string>
    <key>CFBundleTypeRole</key><string>Viewer</string>
    <key>LSHandlerRank</key><string>Alternate</string>
    <key>LSItemContentTypes</key><array><string>public.audio</string></array>
  </dict></array>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>NSHumanReadableCopyright</key><string>MIT License</string>
</dict>
</plist>
PLIST

# 4. ad-hoc signature (no Developer ID needed to run locally)
codesign --force --sign - "$APP/Contents/MacOS/ttsdc"
codesign --force --sign - "$APP"
codesign --verify --strict "$APP"
echo "Built $APP ($VERSION)"
