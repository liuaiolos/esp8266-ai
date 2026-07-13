#!/bin/zsh
set -euo pipefail

# Build a self-contained, drag-installable macOS application bundle from the
# Swift Package executable. The app and its SwiftPM resource bundle must stay
# together: Bundle.module resolves image assets from Contents/Resources.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIGURATION="${1:-release}"
APP_NAME="AIClockBridge"
OUTPUT_DIR="$ROOT/dist"
APP="$OUTPUT_DIR/AI Clock Bridge.app"

cd "$ROOT"
swift build -c "$CONFIGURATION"
BIN_DIR="$(swift build -c "$CONFIGURATION" --show-bin-path)"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BIN_DIR/$APP_NAME" "$APP/Contents/MacOS/$APP_NAME"
cp "$ROOT/Packaging/Info.plist" "$APP/Contents/Info.plist"
cp "$ROOT/Packaging/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
ditto "$BIN_DIR/${APP_NAME}_${APP_NAME}.bundle" \
    "$APP/Contents/Resources/${APP_NAME}_${APP_NAME}.bundle"

# An ad-hoc signature lets macOS validate the bundle locally. It is not
# notarized; Gatekeeper may still ask for confirmation on another Mac.
codesign --force --sign - "$APP"

echo "Created: $APP"
