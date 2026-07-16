#!/bin/zsh
set -euo pipefail

# Build a self-contained, drag-installable universal macOS application bundle
# from the Swift Package executable. The app and its SwiftPM resource bundle
# must stay together: Bundle.module resolves image assets from Contents/Resources.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIGURATION="${1:-release}"
APP_NAME="AIClockBridge"
OUTPUT_DIR="$ROOT/dist"
APP="$OUTPUT_DIR/AI Clock Bridge.app"

cd "$ROOT"

ARCHS=(arm64 x86_64)
declare -A BIN_DIRS
for ARCH in "${ARCHS[@]}"; do
    TRIPLE="${ARCH}-apple-macosx12.0"
    SCRATCH_DIR="$ROOT/.build/universal-${ARCH}"
    swift build -c "$CONFIGURATION" --triple "$TRIPLE" --scratch-path "$SCRATCH_DIR"
    # SwiftPM elides the deployment-version component in its output directory.
    BIN_DIRS[$ARCH]="$SCRATCH_DIR/${ARCH}-apple-macosx/$CONFIGURATION"
done

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

lipo -create \
    "${BIN_DIRS[arm64]}/$APP_NAME" \
    "${BIN_DIRS[x86_64]}/$APP_NAME" \
    -output "$APP/Contents/MacOS/$APP_NAME"
cp "$ROOT/Packaging/Info.plist" "$APP/Contents/Info.plist"
cp "$ROOT/Packaging/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
ditto "${BIN_DIRS[arm64]}/${APP_NAME}_${APP_NAME}.bundle" \
    "$APP/Contents/Resources/${APP_NAME}_${APP_NAME}.bundle"

# An ad-hoc signature lets macOS validate the bundle locally. It is not
# notarized; Gatekeeper may still ask for confirmation on another Mac.
codesign --force --sign - "$APP"

echo "Created: $APP"
echo "Architectures: $(lipo -archs "$APP/Contents/MacOS/$APP_NAME")"
