#!/bin/zsh
set -euo pipefail

PROJECT_ROOT="${0:A:h:h}"
APP_ROOT="$PROJECT_ROOT/dist/iPhone Screen Bridge.app"
CONTENTS="$APP_ROOT/Contents"

cd "$PROJECT_ROOT"
xcrun swift build -c release
mkdir -p "$CONTENTS/MacOS"
cp "$PROJECT_ROOT/.build/release/iphone-screen-bridge" "$CONTENTS/MacOS/iphone-screen-bridge"
cp "$PROJECT_ROOT/Resources/Info.plist" "$CONTENTS/Info.plist"
codesign --force --deep --sign - "$APP_ROOT"

echo "$APP_ROOT"
