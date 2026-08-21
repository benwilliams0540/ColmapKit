#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROBE_SOURCE_DIR="$ROOT_DIR/tests/colmapkit_frame_feature_readiness_probe"
BUILD_ROOT="${COLMAPKIT_READINESS_BUILD_ROOT:-"$ROOT_DIR/build-colmapkit-frame-feature-readiness-probe"}"
RESULT_ROOT="${COLMAPKIT_READINESS_RESULT_ROOT:-"$ROOT_DIR/dist/colmapkit-frame-feature-readiness-probe-build"}"
RC3_ZIP="$ROOT_DIR/dist/colmapkit-v0.3.0-rc3-14eadc5c/ColmapKit.xcframework.zip"
RC3_MATRIX="$ROOT_DIR/doc/colmapkit_frame_feature_rc3_physical_predeclaration.md"
EXPECTED_RC3_ZIP_SHA256=fe7c88966b0f4f3e67d003a0d0160d40a716590db769a465f732ffbc31d48531
EXPECTED_RC3_MATRIX_SHA256=758bed5ae71ea8968a6e4e88c74869459826356633fbba86ebe77d1e62842f92
EXPECTED_RC3_SOURCE=14eadc5cad82eb7a53b52d8739b4345054f15d46
EXPECTED_RC3_RUNTIME=0.3.0-rc.3+14eadc5c
EXPECTED_DEVICE_ID=302F7720-91AC-5103-A977-592E006C1FF7
EXPECTED_DEVICE_UDID=00008103-0012086A1179001E
EXPECTED_PRODUCT_TYPE=iPad13,6
EXPECTED_OS_VERSION=27.0
EXPECTED_OS_BUILD=24A5370h
MODE="${COLMAPKIT_READINESS_MODE:-build}"
XCODE_PROJECT_ROOT="$BUILD_ROOT/xcode"
DERIVED_DATA_ROOT="$BUILD_ROOT/derived-data"
APP_PATH="$XCODE_PROJECT_ROOT/Release-iphoneos/ColmapKitFrameFeatureReadinessProbe.app"
APP_BINARY="$APP_PATH/ColmapKitFrameFeatureReadinessProbe"
BUNDLE_ID=org.colmap.ColmapKitFrameFeatureReadinessProbe

if [[ "$MODE" != "build" && "$MODE" != "physical" ]]; then
  echo "error: COLMAPKIT_READINESS_MODE must be build or physical." >&2
  exit 1
fi
if [[ "$(shasum -a 256 "$RC3_ZIP" | awk '{print $1}')" != "$EXPECTED_RC3_ZIP_SHA256" ]]; then
  echo "error: Frozen RC3 ZIP identity changed." >&2
  exit 1
fi
if [[ "$(shasum -a 256 "$RC3_MATRIX" | awk '{print $1}')" != "$EXPECTED_RC3_MATRIX_SHA256" ]]; then
  echo "error: Frozen RC3 matrix identity changed." >&2
  exit 1
fi
if [[ "$MODE" == "physical" ]]; then
  if [[ "${COLMAPKIT_ALLOW_PHYSICAL_DEVICE:-NO}" != "YES" ]]; then
    echo "error: Physical probe requires COLMAPKIT_ALLOW_PHYSICAL_DEVICE=YES." >&2
    exit 1
  fi
  DEVICE_ID="${COLMAPKIT_DEVICE_ID:?COLMAPKIT_DEVICE_ID is required for physical mode}"
  DEVELOPMENT_TEAM="${COLMAPKIT_DEVELOPMENT_TEAM:?COLMAPKIT_DEVELOPMENT_TEAM is required for physical mode}"
  if [[ "$DEVICE_ID" != "$EXPECTED_DEVICE_ID" ]]; then
    echo "error: Refusing unexpected CoreDevice identifier $DEVICE_ID." >&2
    exit 1
  fi
  if [[ -z "${COLMAPKIT_READINESS_RESULT_ROOT:-}" ]]; then
    echo "error: Physical mode requires a unique COLMAPKIT_READINESS_RESULT_ROOT." >&2
    exit 1
  fi
  if [[ -e "$RESULT_ROOT" ]]; then
    echo "error: Refusing to overwrite readiness probe root: $RESULT_ROOT" >&2
    exit 1
  fi
  if [[ "$(git -C "$ROOT_DIR" branch --show-current)" != "integrate/upstream-main-2026-08" ]] ||
    [[ -n "$(git -C "$ROOT_DIR" status --porcelain --untracked-files=normal)" ]]; then
    echo "error: Physical probe requires the clean canonical owner branch." >&2
    exit 1
  fi
fi

mkdir -p "$BUILD_ROOT" "$RESULT_ROOT"
if [[ "$MODE" == "physical" ]]; then
  ps -A -o pid=,pgid=,%cpu=,rss=,command= > "$RESULT_ROOT/host-processes-before.txt"
  xcrun devicectl device info details --device "$DEVICE_ID" \
    > "$RESULT_ROOT/device-details.txt" 2>&1
  xcrun devicectl device info lockState --device "$DEVICE_ID" \
    > "$RESULT_ROOT/device-lock-state.txt" 2>&1
  xcrun devicectl device info processes --device "$DEVICE_ID" \
    > "$RESULT_ROOT/device-processes-before.txt" 2>&1
  grep -Fq "Identifier: $EXPECTED_DEVICE_ID" "$RESULT_ROOT/device-details.txt"
  grep -Fq "UDID: $EXPECTED_DEVICE_UDID" "$RESULT_ROOT/device-details.txt"
  grep -Fq "Product Type: $EXPECTED_PRODUCT_TYPE" "$RESULT_ROOT/device-details.txt"
  grep -Fq "OS Version: $EXPECTED_OS_VERSION" "$RESULT_ROOT/device-details.txt"
  grep -Fq "OS Build Update: $EXPECTED_OS_BUILD" "$RESULT_ROOT/device-details.txt"
  grep -Fq "Transport Type: wired" "$RESULT_ROOT/device-details.txt"
  grep -Fq "Pairing State: paired" "$RESULT_ROOT/device-details.txt"
  grep -Fq "Boot State: booted" "$RESULT_ROOT/device-details.txt"
  grep -Eiq "unlocked|passcode.*false" "$RESULT_ROOT/device-lock-state.txt"
  if grep -Eq 'ColmapKitFrameFeatureDeviceHarness|BrushKit.*Harness|msplat.*benchmark' \
    "$RESULT_ROOT/host-processes-before.txt"; then
    echo "error: A competing host benchmark process is active." >&2
    exit 1
  fi
  if grep -Eq 'ColmapKitFrameFeatureDeviceHarness|BrushKit.*Harness|msplat.*benchmark' \
    "$RESULT_ROOT/device-processes-before.txt"; then
    echo "error: A competing device benchmark process is active." >&2
    exit 1
  fi
fi

cmake \
  -S "$PROBE_SOURCE_DIR" \
  -B "$XCODE_PROJECT_ROOT" \
  -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 \
  -DCMAKE_XCODE_GENERATE_SCHEME=ON \
  -DCOLMAPKIT_DEVELOPMENT_TEAM="${COLMAPKIT_DEVELOPMENT_TEAM:-}"

if [[ "$MODE" == "build" ]]; then
  xcodebuild \
    -project "$XCODE_PROJECT_ROOT/ColmapKitFrameFeatureReadinessProbe.xcodeproj" \
    -scheme ColmapKitFrameFeatureReadinessProbe \
    -configuration Release \
    -destination 'generic/platform=iOS' \
    -derivedDataPath "$DERIVED_DATA_ROOT" \
    CODE_SIGNING_ALLOWED=NO \
    build > "$RESULT_ROOT/xcodebuild.log" 2>&1
else
  xcodebuild \
    -project "$XCODE_PROJECT_ROOT/ColmapKitFrameFeatureReadinessProbe.xcodeproj" \
    -scheme ColmapKitFrameFeatureReadinessProbe \
    -configuration Release \
    -destination "id=$DEVICE_ID" \
    -derivedDataPath "$DERIVED_DATA_ROOT" \
    DEVELOPMENT_TEAM="$DEVELOPMENT_TEAM" \
    -allowProvisioningUpdates \
    build > "$RESULT_ROOT/xcodebuild.log" 2>&1
fi

if [[ -d "$APP_PATH/Frameworks/ColmapKit.framework" ]]; then
  echo "error: Readiness probe unexpectedly embeds ColmapKit." >&2
  exit 1
fi
if otool -L "$APP_BINARY" | tail -n +2 | grep -Fq ColmapKit; then
  echo "error: Readiness probe unexpectedly links ColmapKit." >&2
  exit 1
fi
if nm -u "$APP_BINARY" | grep -Eq 'ColmapKit(Start|Create|Wait|Cancel|Release)'; then
  echo "error: Readiness probe has a ColmapKit job API reference." >&2
  exit 1
fi
if rg -n 'ColmapKit(Start|Create|Wait|Cancel|Release)|run_colmapkit_frame_feature_device_harness' \
  "$PROBE_SOURCE_DIR"; then
  echo "error: Readiness probe source can reach engine or frozen acceptance harness work." >&2
  exit 1
fi

{
  printf 'mode=%s\n' "$MODE"
  printf 'probe_source_commit=%s\n' "$(git -C "$ROOT_DIR" rev-parse HEAD)"
  printf 'probe_binary_sha256=%s\n' "$(shasum -a 256 "$APP_BINARY" | awk '{print $1}')"
  printf 'rc3_source=%s\n' "$EXPECTED_RC3_SOURCE"
  printf 'rc3_runtime=%s\n' "$EXPECTED_RC3_RUNTIME"
  printf 'rc3_zip_sha256=%s\n' "$EXPECTED_RC3_ZIP_SHA256"
  printf 'rc3_matrix_sha256=%s\n' "$EXPECTED_RC3_MATRIX_SHA256"
  printf 'colmapkit_embedded=false\n'
  printf 'colmapkit_linked=false\n'
  printf 'frozen_acceptance_harness_reachable=false\n'
} > "$RESULT_ROOT/build-receipt.txt"

if [[ "$MODE" == "build" ]]; then
  echo "Build-only readiness proof passed; no device command was issued."
  echo "Wrote $RESULT_ROOT/build-receipt.txt"
  exit 0
fi

xcrun devicectl device install app --device "$DEVICE_ID" "$APP_PATH" \
  > "$RESULT_ROOT/install.log" 2>&1
set +e
xcrun devicectl device process launch \
  --device "$DEVICE_ID" \
  --console \
  --terminate-existing \
  "$BUNDLE_ID" > "$RESULT_ROOT/runtime.log" 2>&1
launch_status=$?
set -e
if [[ "$launch_status" -ne 0 ]]; then
  echo "error: Readiness probe launch failed with status $launch_status." >&2
  exit "$launch_status"
fi
if ! grep -Fq 'COLMAPKIT_FRAME_FEATURE_READINESS_RESULT=' "$RESULT_ROOT/runtime.log"; then
  echo "error: Readiness probe emitted no structured result." >&2
  exit 1
fi
grep -F 'COLMAPKIT_FRAME_FEATURE_READINESS_RESULT=' "$RESULT_ROOT/runtime.log" \
  | tail -n 1 \
  | sed 's/^.*COLMAPKIT_FRAME_FEATURE_READINESS_RESULT=//' \
  > "$RESULT_ROOT/runtime-readiness.json"
jq -e '.schemaVersion == 1 and .engineApiCallCount == 0 and .acceptanceMatrixLaunchAttempted == false and .terminatesAfterReadiness == true' \
  "$RESULT_ROOT/runtime-readiness.json" >/dev/null

jq -n \
  --arg probeSourceCommit "$(git -C "$ROOT_DIR" rev-parse HEAD)" \
  --arg probeBinarySHA256 "$(shasum -a 256 "$APP_BINARY" | awk '{print $1}')" \
  --arg rc3Source "$EXPECTED_RC3_SOURCE" \
  --arg rc3Runtime "$EXPECTED_RC3_RUNTIME" \
  --arg rc3ZipSHA256 "$EXPECTED_RC3_ZIP_SHA256" \
  --arg rc3MatrixSHA256 "$EXPECTED_RC3_MATRIX_SHA256" \
  --arg coreDeviceIdentifier "$EXPECTED_DEVICE_ID" \
  --arg udid "$EXPECTED_DEVICE_UDID" \
  --arg productType "$EXPECTED_PRODUCT_TYPE" \
  --arg osVersion "$EXPECTED_OS_VERSION" \
  --arg osBuild "$EXPECTED_OS_BUILD" \
  --slurpfile runtime "$RESULT_ROOT/runtime-readiness.json" \
  '{
    schemaVersion: 1,
    operation: "colmapkit_frame_feature_physical_readiness_probe_v1",
    probe: {
      sourceCommit: $probeSourceCommit,
      binarySHA256: $probeBinarySHA256,
      colmapkitEmbedded: false,
      colmapkitLinked: false,
      frozenAcceptanceHarnessReachable: false
    },
    rc3Reference: {
      sourceCommit: $rc3Source,
      runtimeVersion: $rc3Runtime,
      zipSHA256: $rc3ZipSHA256,
      physicalMatrixSHA256: $rc3MatrixSHA256,
      packageUsedByProbe: false
    },
    hostVerifiedDevice: {
      coreDeviceIdentifier: $coreDeviceIdentifier,
      udid: $udid,
      productType: $productType,
      osVersion: $osVersion,
      osBuild: $osBuild
    },
    runtime: $runtime[0]
  }' > "$RESULT_ROOT/readiness-receipt.json"

shasum -a 256 \
  "$RESULT_ROOT/build-receipt.txt" \
  "$RESULT_ROOT/device-details.txt" \
  "$RESULT_ROOT/device-lock-state.txt" \
  "$RESULT_ROOT/device-processes-before.txt" \
  "$RESULT_ROOT/host-processes-before.txt" \
  "$RESULT_ROOT/install.log" \
  "$RESULT_ROOT/runtime.log" \
  "$RESULT_ROOT/runtime-readiness.json" \
  "$RESULT_ROOT/readiness-receipt.json" \
  > "$RESULT_ROOT/file-hashes.txt"
shasum -a 256 "$RESULT_ROOT/readiness-receipt.json" > "$RESULT_ROOT/readiness-receipt.sha256"

echo "Readiness probe completed once on authorized device $DEVICE_ID."
echo "Wrote $RESULT_ROOT/readiness-receipt.json"
