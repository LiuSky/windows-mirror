#!/bin/zsh
set -euo pipefail

PROJECT_ROOT="${0:A:h:h}"
APP="$PROJECT_ROOT/dist/iPhone Screen Bridge.app"

if [[ ! -x "$APP/Contents/MacOS/iphone-screen-bridge" ]]; then
  "$PROJECT_ROOT/scripts/build-macos-app.sh"
fi

cd "$PROJECT_ROOT"
open "$APP"
