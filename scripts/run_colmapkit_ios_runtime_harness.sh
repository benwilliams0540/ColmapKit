#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XCFRAMEWORK_PATH="${COLMAPKIT_XCFRAMEWORK_PATH:-"$ROOT_DIR/dist/colmapkit-apple/ColmapKit.xcframework"}"
BUILD_ROOT="${COLMAPKIT_RUNTIME_BUILD_ROOT:-"$ROOT_DIR/build-colmapkit-ios-runtime"}"
RESULT_ROOT="${COLMAPKIT_RUNTIME_RESULT_ROOT:-"$ROOT_DIR/dist/colmapkit-ios-runtime"}"
SIMULATOR_UDID="${SIMULATOR_UDID:-}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-18.0}"
BUNDLE_ID=org.colmap.ColmapKitRuntimeHarness
APP_PATH="$BUILD_ROOT/ColmapKitRuntimeHarness.app"
EXECUTABLE_PATH="$APP_PATH/ColmapKitRuntimeHarness"
FRAMEWORK_SLICE="$XCFRAMEWORK_PATH/ios-arm64-simulator/ColmapKit.framework"
FIXTURE_RUN_ROOT="$BUILD_ROOT/fixture-run"
FIXTURE_IMAGE_DIR="${COLMAPKIT_RUNTIME_FIXTURE_IMAGE_DIR:-}"
LOG_PATH="$RESULT_ROOT/runtime.log"

if [[ ! -d "$FRAMEWORK_SLICE" ]]; then
  echo "error: Missing iOS Simulator framework slice: $FRAMEWORK_SLICE" >&2
  exit 1
fi

rm -rf "$BUILD_ROOT" "$RESULT_ROOT"
mkdir -p "$APP_PATH/Frameworks" "$RESULT_ROOT"

if [[ -n "$FIXTURE_IMAGE_DIR" ]]; then
  if [[ ! -d "$FIXTURE_IMAGE_DIR" ]]; then
    echo "error: Runtime fixture directory does not exist: $FIXTURE_IMAGE_DIR" >&2
    exit 1
  fi
  ditto "$FIXTURE_IMAGE_DIR" "$APP_PATH/Fixture"
else
  python3 "$ROOT_DIR/scripts/python/colmapkit_compare.py" \
    --run-dir "$FIXTURE_RUN_ROOT" \
    --generate-synthetic-fixture \
    --generate-only \
    --fixture-image-count 8
  ditto "$FIXTURE_RUN_ROOT/fixture/images" "$APP_PATH/Fixture"
fi
ditto "$FRAMEWORK_SLICE" "$APP_PATH/Frameworks/ColmapKit.framework"
cp "$ROOT_DIR/tests/colmapkit_ios_runtime/Info.plist" "$APP_PATH/Info.plist"

SIMULATOR_SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"
MODULE_CACHE="$BUILD_ROOT/module-cache"
mkdir -p "$MODULE_CACHE"
xcrun --sdk iphonesimulator swiftc \
  -module-cache-path "$MODULE_CACHE" \
  -Xcc "-fmodules-cache-path=$MODULE_CACHE" \
  -sdk "$SIMULATOR_SDK" \
  -target "arm64-apple-ios${IOS_DEPLOYMENT_TARGET}-simulator" \
  -F "$XCFRAMEWORK_PATH/ios-arm64-simulator" \
  "$ROOT_DIR/tests/colmapkit_ios_runtime/main.swift" \
  -framework ColmapKit \
  -framework UIKit \
  -Xlinker -rpath \
  -Xlinker @executable_path/Frameworks \
  -o "$EXECUTABLE_PATH"

codesign --force --deep --sign - "$APP_PATH"
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

if [[ -z "$SIMULATOR_UDID" ]]; then
  SIMULATOR_UDID="$(
    xcrun simctl list devices available |
      sed -nE 's/.*\(([0-9A-Fa-f-]{36})\) \((Booted|Shutdown)\)[[:space:]]*$/\1/p' |
      head -n 1
  )"
fi
if [[ -z "$SIMULATOR_UDID" ]]; then
  echo "error: No available iOS Simulator device was found." >&2
  exit 1
fi

if ! xcrun simctl list devices | grep -F "$SIMULATOR_UDID" | grep -Fq '(Booted)'; then
  xcrun simctl boot "$SIMULATOR_UDID"
fi
xcrun simctl bootstatus "$SIMULATOR_UDID" -b
xcrun simctl install "$SIMULATOR_UDID" "$APP_PATH"

set +e
xcrun simctl launch --console-pty --terminate-running-process "$SIMULATOR_UDID" "$BUNDLE_ID" 2>&1 | tee "$LOG_PATH"
launch_status=${PIPESTATUS[0]}
set -e
if [[ "$launch_status" -ne 0 ]]; then
  echo "error: Runtime harness exited with status $launch_status." >&2
  exit "$launch_status"
fi
if ! grep -Fq 'COLMAPKIT_RUNTIME_RESULT=' "$LOG_PATH"; then
  echo "error: Runtime harness did not emit a successful result." >&2
  exit 1
fi

grep -F 'COLMAPKIT_RUNTIME_RESULT=' "$LOG_PATH" | tail -n 1 | sed 's/^.*COLMAPKIT_RUNTIME_RESULT=//' > "$RESULT_ROOT/result.json"
python3 -m json.tool "$RESULT_ROOT/result.json" > "$RESULT_ROOT/result.pretty.json"

echo "Runtime harness passed on Simulator $SIMULATOR_UDID"
echo "Wrote $RESULT_ROOT/result.json"
