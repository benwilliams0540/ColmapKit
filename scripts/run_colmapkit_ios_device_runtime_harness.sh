#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XCFRAMEWORK_PATH="${COLMAPKIT_XCFRAMEWORK_PATH:-"$ROOT_DIR/dist/colmapkit-apple/ColmapKit.xcframework"}"
BUILD_ROOT="${COLMAPKIT_DEVICE_BUILD_ROOT:-"$ROOT_DIR/build-colmapkit-ios-device-runtime"}"
RESULT_ROOT="${COLMAPKIT_DEVICE_RESULT_ROOT:-"$ROOT_DIR/dist/colmapkit-ios-device-runtime"}"
DEVICE_ID="${COLMAPKIT_DEVICE_ID:?COLMAPKIT_DEVICE_ID is required}"
DEVELOPMENT_TEAM="${COLMAPKIT_DEVELOPMENT_TEAM:?COLMAPKIT_DEVELOPMENT_TEAM is required}"
BUNDLE_ID=org.colmap.ColmapKitRuntimeHarness
FRAMEWORK_PATH="$XCFRAMEWORK_PATH/ios-arm64/ColmapKit.framework"
FIXTURE_RUN_ROOT="$BUILD_ROOT/fixture-run"
FIXTURE_DIR="$BUILD_ROOT/Fixture"
STRICT_FIXTURE_DIR="$BUILD_ROOT/StrictFixture"
XCODE_PROJECT_ROOT="$BUILD_ROOT/xcode"
DERIVED_DATA_ROOT="$BUILD_ROOT/derived-data"
APP_PATH="$XCODE_PROJECT_ROOT/Release-iphoneos/ColmapKitRuntimeHarness.app"
LOG_PATH="$RESULT_ROOT/runtime.log"

if [[ ! -d "$FRAMEWORK_PATH" ]]; then
  echo "error: Missing iOS framework slice: $FRAMEWORK_PATH" >&2
  exit 1
fi

rm -rf "$BUILD_ROOT" "$RESULT_ROOT"
mkdir -p "$BUILD_ROOT" "$RESULT_ROOT"

python3 "$ROOT_DIR/scripts/python/colmapkit_compare.py" \
  --run-dir "$FIXTURE_RUN_ROOT" \
  --generate-synthetic-fixture \
  --generate-only \
  --fixture-image-count 8
ditto "$FIXTURE_RUN_ROOT/fixture/images" "$STRICT_FIXTURE_DIR"
ditto "$FIXTURE_RUN_ROOT/fixture/images" "$FIXTURE_DIR"
for fixture_image in "$FIXTURE_DIR"/*.pgm; do
  sips --flip vertical "$fixture_image" >/dev/null
done

cmake \
  -S "$ROOT_DIR/tests/colmapkit_ios_runtime" \
  -B "$XCODE_PROJECT_ROOT" \
  -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 \
  -DCMAKE_XCODE_GENERATE_SCHEME=ON \
  -DCOLMAPKIT_FRAMEWORK="$FRAMEWORK_PATH" \
  -DCOLMAPKIT_RUNTIME_FIXTURE_DIR="$FIXTURE_DIR" \
  -DCOLMAPKIT_RUNTIME_STRICT_FIXTURE_DIR="$STRICT_FIXTURE_DIR" \
  -DCOLMAPKIT_DEVELOPMENT_TEAM="$DEVELOPMENT_TEAM"

xcodebuild \
  -project "$XCODE_PROJECT_ROOT/ColmapKitRuntimeHarness.xcodeproj" \
  -scheme ColmapKitRuntimeHarness \
  -configuration Release \
  -destination "id=$DEVICE_ID" \
  -derivedDataPath "$DERIVED_DATA_ROOT" \
  -allowProvisioningUpdates \
  build

xcrun devicectl device install app \
  --device "$DEVICE_ID" \
  "$APP_PATH"

set +e
xcrun devicectl device process launch \
  --device "$DEVICE_ID" \
  --console \
  --terminate-existing \
  "$BUNDLE_ID" 2>&1 | tee "$LOG_PATH"
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

grep -F 'COLMAPKIT_RUNTIME_RESULT=' "$LOG_PATH" \
  | tail -n 1 \
  | sed 's/^.*COLMAPKIT_RUNTIME_RESULT=//' \
  > "$RESULT_ROOT/result.json"
python3 -m json.tool "$RESULT_ROOT/result.json" \
  > "$RESULT_ROOT/result.pretty.json"

echo "Runtime harness passed on device $DEVICE_ID"
echo "Wrote $RESULT_ROOT/result.json"
