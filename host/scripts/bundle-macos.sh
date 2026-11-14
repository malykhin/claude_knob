#!/bin/sh
# Builds "Claude Knob.app" (menu-bar app around knobd) in host/target/release/.
# UNIVERSAL=1 builds an arm64 + x86_64 binary (used for releases).
set -eu
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
APP="target/release/Claude Knob.app"

if [ "${UNIVERSAL:-0}" = 1 ]; then
  cargo build --release --target aarch64-apple-darwin --target x86_64-apple-darwin
  mkdir -p target/release
  lipo -create -output target/release/knobd \
    target/aarch64-apple-darwin/release/knobd target/x86_64-apple-darwin/release/knobd
else
  cargo build --release
fi

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp target/release/knobd "$APP/Contents/MacOS/knobd"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Claude Knob</string>
  <key>CFBundleDisplayName</key><string>Claude Knob</string>
  <key>CFBundleIdentifier</key><string>com.claudeknob.knobd</string>
  <key>CFBundleExecutable</key><string>knobd</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSUIElement</key><true/>
  <key>NSAppleEventsUsageDescription</key>
  <string>Claude Knob brings the Terminal window of the selected Claude Code session to the front.</string>
</dict>
</plist>
PLIST

# Ad-hoc signature so macOS can attach the Automation permission to the app.
codesign --force --sign - "$APP"
echo "Built $APP"
echo "Move it to /Applications, open it, then use the menu to install hooks and enable Start at login."
