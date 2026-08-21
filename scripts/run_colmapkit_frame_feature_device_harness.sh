#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XCFRAMEWORK_PATH="${COLMAPKIT_XCFRAMEWORK_PATH:-"$ROOT_DIR/dist/colmapkit-v0.3.0-rc1-c69711b8/ColmapKit.xcframework"}"
BUILD_ROOT="${COLMAPKIT_DEVICE_BUILD_ROOT:-"$ROOT_DIR/build-colmapkit-frame-feature-device"}"
RESULT_ROOT="${COLMAPKIT_DEVICE_RESULT_ROOT:-"$ROOT_DIR/dist/colmapkit-frame-feature-device"}"
ZIP_PATH="$(dirname "$XCFRAMEWORK_PATH")/ColmapKit.xcframework.zip"
MATRIX_PATH="$(dirname "$XCFRAMEWORK_PATH")/deferred-physical-acceptance-matrix.md"
FIXTURE_DIR="$ROOT_DIR/tests/colmapkit_frame_feature_device/Fixture"
FRAMEWORK_PATH="$XCFRAMEWORK_PATH/ios-arm64/ColmapKit.framework"
FRAMEWORK_BINARY="$FRAMEWORK_PATH/ColmapKit"
EXPECTED_FRAMEWORK_SHA256=8876954755d656e1968426d411c24ac48e87d1903ee5f449126f042f6a704eb0
EXPECTED_ZIP_SHA256=e7e69b029715bfe4f63551c05fdc9851ef565844f216669b5e0fbea5ba8a5aa3
EXPECTED_MATRIX_SHA256=47071bd7ef9a2a94dab5b991faec97e5f998646049514c1cfd3582262c6f086d
EXPECTED_RELEASE=0.3.0-rc.1+c69711b8
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
  echo "error: Missing exact RC1 iPhoneOS framework: $FRAMEWORK_BINARY" >&2
  exit 1
fi
actual_framework_sha256="$(shasum -a 256 "$FRAMEWORK_BINARY" | awk '{print $1}')"
if [[ "$actual_framework_sha256" != "$EXPECTED_FRAMEWORK_SHA256" ]]; then
  echo "error: RC1 iPhoneOS framework hash mismatch." >&2
  exit 1
fi
if [[ ! -f "$FIXTURE_DIR/fixture-manifest.json" ]]; then
  echo "error: Missing committed frame-feature fixture manifest." >&2
  exit 1
fi
if [[ "$(shasum -a 256 "$ZIP_PATH" | awk '{print $1}')" != "$EXPECTED_ZIP_SHA256" ]]; then
  echo "error: RC1 ZIP hash mismatch." >&2
  exit 1
fi
if [[ "$(shasum -a 256 "$MATRIX_PATH" | awk '{print $1}')" != "$EXPECTED_MATRIX_SHA256" ]]; then
  echo "error: RC1 physical matrix hash mismatch." >&2
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
    echo "error: Embedded framework differs from RC1 beyond the required signature envelope." >&2
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
