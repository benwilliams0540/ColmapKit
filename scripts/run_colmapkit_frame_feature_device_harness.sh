#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XCFRAMEWORK_PATH="${COLMAPKIT_XCFRAMEWORK_PATH:-"$ROOT_DIR/dist/colmapkit-v0.3.0-rc2-670a14bb/ColmapKit.xcframework"}"
BUILD_ROOT="${COLMAPKIT_DEVICE_BUILD_ROOT:-"$ROOT_DIR/build-colmapkit-frame-feature-device"}"
RESULT_ROOT="${COLMAPKIT_DEVICE_RESULT_ROOT:-"$ROOT_DIR/dist/colmapkit-frame-feature-device"}"
ZIP_PATH="$(dirname "$XCFRAMEWORK_PATH")/ColmapKit.xcframework.zip"
MATRIX_PATH="${COLMAPKIT_DEVICE_MATRIX_PATH:-"$ROOT_DIR/doc/colmapkit_frame_feature_rc2_physical_predeclaration.md"}"
FIXTURE_DIR="$ROOT_DIR/tests/colmapkit_frame_feature_device/Fixture"
FRAMEWORK_PATH="$XCFRAMEWORK_PATH/ios-arm64/ColmapKit.framework"
FRAMEWORK_BINARY="$FRAMEWORK_PATH/ColmapKit"
EXPECTED_FRAMEWORK_SHA256=7e4dbc6709d0b698e4dcc783c302c87534e2b2f81f8cb43ee45463b18f2cc4ed
EXPECTED_ZIP_SHA256=2ff308666327193012cb9b6d2974d3d58ecb3e1b2714e885bdb52c3e8cf2bf70
EXPECTED_MATRIX_SHA256=f657d2705d3870a76e4ed50fdd3ef223f9c3aa888a8fed20e8714ecfcdea6fff
EXPECTED_RELEASE=0.3.0-rc.2+670a14bb
EXPECTED_ADMISSION_BUDGET_BYTES=335544320
MODE="${COLMAPKIT_DEVICE_MODE:-build}"
XCODE_PROJECT_ROOT="$BUILD_ROOT/xcode"
DERIVED_DATA_ROOT="$BUILD_ROOT/derived-data"
APP_PATH="$XCODE_PROJECT_ROOT/Release-iphoneos/ColmapKitFrameFeatureDeviceHarness.app"
BUNDLE_ID=org.colmap.ColmapKitFrameFeatureDeviceHarness

signature_normalized_sha256() {
  local source="$1"
  local copy
  copy="$(mktemp /private/tmp/colmapkit-signature-normalized.XXXXXX)"
  cp "$source" "$copy"
  codesign --remove-signature "$copy" >/dev/null 2>&1
  shasum -a 256 "$copy" | awk '{print $1}'
  rm -f "$copy"
}

if [[ ! -f "$FRAMEWORK_BINARY" ]]; then
  echo "error: Missing exact RC2 iPhoneOS framework: $FRAMEWORK_BINARY" >&2
  exit 1
fi
actual_framework_sha256="$(shasum -a 256 "$FRAMEWORK_BINARY" | awk '{print $1}')"
if [[ "$actual_framework_sha256" != "$EXPECTED_FRAMEWORK_SHA256" ]]; then
  echo "error: RC2 iPhoneOS framework hash mismatch." >&2
  exit 1
fi
if [[ ! -f "$FIXTURE_DIR/fixture-manifest.json" ]]; then
  echo "error: Missing committed frame-feature fixture manifest." >&2
  exit 1
fi
if [[ "$(shasum -a 256 "$ZIP_PATH" | awk '{print $1}')" != "$EXPECTED_ZIP_SHA256" ]]; then
  echo "error: RC2 ZIP hash mismatch." >&2
  exit 1
fi
if [[ "$(shasum -a 256 "$MATRIX_PATH" | awk '{print $1}')" != "$EXPECTED_MATRIX_SHA256" ]]; then
  echo "error: RC2 physical matrix hash mismatch." >&2
  exit 1
fi
if [[ "$MODE" != "build" && "$MODE" != "physical" ]]; then
  echo "error: COLMAPKIT_DEVICE_MODE must be build or physical." >&2
  exit 1
fi
if [[ "$MODE" == "physical" ]]; then
  if [[ "${COLMAPKIT_ALLOW_PHYSICAL_DEVICE:-NO}" != "YES" ]]; then
    echo "error: Physical execution requires COLMAPKIT_ALLOW_PHYSICAL_DEVICE=YES." >&2
    exit 1
  fi
  DEVICE_ID="${COLMAPKIT_DEVICE_ID:?COLMAPKIT_DEVICE_ID is required for physical mode}"
  DEVELOPMENT_TEAM="${COLMAPKIT_DEVELOPMENT_TEAM:?COLMAPKIT_DEVELOPMENT_TEAM is required for physical mode}"
  if [[ -z "${COLMAPKIT_DEVICE_RESULT_ROOT:-}" ]]; then
    echo "error: Physical mode requires a unique COLMAPKIT_DEVICE_RESULT_ROOT." >&2
    exit 1
  fi
  if [[ -e "$RESULT_ROOT" ]]; then
    echo "error: Refusing to overwrite physical attempt directory: $RESULT_ROOT" >&2
    exit 1
  fi
fi

mkdir -p "$BUILD_ROOT" "$RESULT_ROOT"
if [[ "$MODE" == "physical" ]]; then
  xcrun devicectl device info details --device "$DEVICE_ID" \
    > "$RESULT_ROOT/device-details.txt" 2>&1
  xcrun devicectl device info lockState --device "$DEVICE_ID" \
    > "$RESULT_ROOT/device-lock-state.txt" 2>&1
  xcrun devicectl device info processes --device "$DEVICE_ID" \
    > "$RESULT_ROOT/device-processes-before.txt" 2>&1
fi
cmake \
  -S "$ROOT_DIR/tests/colmapkit_frame_feature_device" \
  -B "$XCODE_PROJECT_ROOT" \
  -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 \
  -DCMAKE_XCODE_GENERATE_SCHEME=ON \
  -DCOLMAPKIT_FRAMEWORK="$FRAMEWORK_PATH" \
  -DCOLMAPKIT_FRAME_FEATURE_FIXTURE_DIR="$FIXTURE_DIR" \
  -DCOLMAPKIT_DEVELOPMENT_TEAM="${COLMAPKIT_DEVELOPMENT_TEAM:-}"

set +e
if [[ "$MODE" == "build" ]]; then
  xcodebuild \
    -project "$XCODE_PROJECT_ROOT/ColmapKitFrameFeatureDeviceHarness.xcodeproj" \
    -scheme ColmapKitFrameFeatureDeviceHarness \
    -configuration Release \
    -destination 'generic/platform=iOS' \
    -derivedDataPath "$DERIVED_DATA_ROOT" \
    CODE_SIGNING_ALLOWED=NO \
    build > "$RESULT_ROOT/xcodebuild.log" 2>&1
  build_status=$?
else
  xcodebuild \
    -project "$XCODE_PROJECT_ROOT/ColmapKitFrameFeatureDeviceHarness.xcodeproj" \
    -scheme ColmapKitFrameFeatureDeviceHarness \
    -configuration Release \
    -destination "id=$DEVICE_ID" \
    -derivedDataPath "$DERIVED_DATA_ROOT" \
    DEVELOPMENT_TEAM="$DEVELOPMENT_TEAM" \
    -allowProvisioningUpdates \
    build > "$RESULT_ROOT/xcodebuild.log" 2>&1
  build_status=$?
fi
set -e
if [[ "$build_status" -ne 0 ]]; then
  printf 'stage=xcodebuild\nstatus=%s\n' "$build_status" > "$RESULT_ROOT/failure-receipt.txt"
  echo "error: Harness build failed; preserved $RESULT_ROOT/xcodebuild.log" >&2
  exit "$build_status"
fi

embedded_framework="$APP_PATH/Frameworks/ColmapKit.framework/ColmapKit"
embedded_sha256="$(shasum -a 256 "$embedded_framework" | awk '{print $1}')"
framework_normalized_sha256="$(signature_normalized_sha256 "$FRAMEWORK_BINARY")"
embedded_normalized_sha256="$(signature_normalized_sha256 "$embedded_framework")"
if [[ "$embedded_sha256" != "$EXPECTED_FRAMEWORK_SHA256" ]]; then
  if [[ "$embedded_normalized_sha256" != "$framework_normalized_sha256" ]]; then
    {
      printf 'stage=embedded-framework-identity\n'
      printf 'expected_sha256=%s\n' "$EXPECTED_FRAMEWORK_SHA256"
      printf 'actual_sha256=%s\n' "$embedded_sha256"
      printf 'expected_signature_normalized_sha256=%s\n' "$framework_normalized_sha256"
      printf 'actual_signature_normalized_sha256=%s\n' "$embedded_normalized_sha256"
    } > "$RESULT_ROOT/failure-receipt.txt"
    echo "error: Embedded framework differs from RC2 beyond the required signature envelope." >&2
    exit 1
  fi
fi

{
  printf 'mode=%s\n' "$MODE"
  printf 'release=%s\n' "$EXPECTED_RELEASE"
  printf 'framework=%s\n' "$FRAMEWORK_PATH"
  printf 'framework_sha256=%s\n' "$actual_framework_sha256"
  printf 'embedded_framework_sha256=%s\n' "$embedded_sha256"
  printf 'framework_signature_normalized_sha256=%s\n' "$framework_normalized_sha256"
  printf 'embedded_signature_normalized_sha256=%s\n' "$embedded_normalized_sha256"
  printf 'zip_sha256=%s\n' "$EXPECTED_ZIP_SHA256"
  printf 'matrix_sha256=%s\n' "$EXPECTED_MATRIX_SHA256"
  printf 'memory_admission_budget_bytes=%s\n' "$EXPECTED_ADMISSION_BUDGET_BYTES"
  printf 'fixture_manifest_sha256=%s\n' \
    "$(shasum -a 256 "$FIXTURE_DIR/fixture-manifest.json" | awk '{print $1}')"
  printf 'app=%s\n' "$APP_PATH"
  printf 'app_size_bytes=%s\n' "$(du -sk "$APP_PATH" | awk '{print $1 * 1024}')"
} > "$RESULT_ROOT/build-receipt.txt"

if [[ "$MODE" == "build" ]]; then
  echo "Build-only audit passed; no device command was issued."
  echo "Wrote $RESULT_ROOT/build-receipt.txt"
  exit 0
fi

LOG_PATH="$RESULT_ROOT/runtime.log"
xcrun devicectl device install app --device "$DEVICE_ID" "$APP_PATH" \
  > "$RESULT_ROOT/install.log" 2>&1
set +e
xcrun devicectl device process launch \
  --device "$DEVICE_ID" \
  --console \
  --terminate-existing \
  "$BUNDLE_ID" 2>&1 | tee "$LOG_PATH"
launch_status=${PIPESTATUS[0]}
set -e
xcrun devicectl device info lockState --device "$DEVICE_ID" \
  > "$RESULT_ROOT/device-lock-state-after.txt" 2>&1
xcrun devicectl device info processes --device "$DEVICE_ID" \
  > "$RESULT_ROOT/device-processes-after.txt" 2>&1
if grep -Fq 'COLMAPKIT_FRAME_FEATURE_DEVICE_PREFLIGHT=' "$LOG_PATH"; then
  grep -F 'COLMAPKIT_FRAME_FEATURE_DEVICE_PREFLIGHT=' "$LOG_PATH" \
    | tail -n 1 \
    | sed 's/^.*COLMAPKIT_FRAME_FEATURE_DEVICE_PREFLIGHT=//' \
    > "$RESULT_ROOT/preflight.json"
  python3 -m json.tool "$RESULT_ROOT/preflight.json" \
    > "$RESULT_ROOT/preflight.pretty.json"
fi
if grep -Fq 'COLMAPKIT_FRAME_FEATURE_DEVICE_FAILURE=' "$LOG_PATH"; then
  grep -F 'COLMAPKIT_FRAME_FEATURE_DEVICE_FAILURE=' "$LOG_PATH" \
    | tail -n 1 \
    | sed 's/^.*COLMAPKIT_FRAME_FEATURE_DEVICE_FAILURE=//' \
    > "$RESULT_ROOT/failure.json"
  python3 -m json.tool "$RESULT_ROOT/failure.json" \
    > "$RESULT_ROOT/failure.pretty.json"
fi
if [[ "$launch_status" -ne 0 ]]; then
  echo "error: Frame-feature harness exited with status $launch_status." >&2
  exit "$launch_status"
fi
if ! grep -Fq 'COLMAPKIT_FRAME_FEATURE_DEVICE_RESULT=' "$LOG_PATH"; then
  echo "error: Harness did not emit a successful result." >&2
  exit 1
fi
grep -F 'COLMAPKIT_FRAME_FEATURE_DEVICE_RESULT=' "$LOG_PATH" \
  | tail -n 1 \
  | sed 's/^.*COLMAPKIT_FRAME_FEATURE_DEVICE_RESULT=//' \
  > "$RESULT_ROOT/result.json"
python3 -m json.tool "$RESULT_ROOT/result.json" > "$RESULT_ROOT/result.pretty.json"
echo "Physical harness completed on explicitly authorized device $DEVICE_ID"
echo "Wrote $RESULT_ROOT/result.json"
