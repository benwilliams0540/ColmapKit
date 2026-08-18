#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${COLMAPKIT_APPLE_BUILD_ROOT:-"$ROOT_DIR/build-colmapkit-apple"}"
DIST_ROOT="${COLMAPKIT_APPLE_DIST_ROOT:-"$ROOT_DIR/dist/colmapkit-apple"}"
MACOS_PACKAGE_ROOT="$BUILD_ROOT/macos-package"
IOS_PACKAGE_ROOT="$BUILD_ROOT/ios-package"
IOS_PROBE_ROOT="$BUILD_ROOT/ios-probe"
XCFRAMEWORK_PATH="$DIST_ROOT/ColmapKit.xcframework"
ZIP_PATH="$DIST_ROOT/ColmapKit.xcframework.zip"
AUDIT_ROOT="$DIST_ROOT/audits"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-15.0}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-18.0}"
COLMAPKIT_BUILD_MACOS="${COLMAPKIT_BUILD_MACOS:-ON}"
COLMAPKIT_BUILD_IOS="${COLMAPKIT_BUILD_IOS:-ON}"
COLMAPKIT_BUILD_LIBOMP="${COLMAPKIT_BUILD_LIBOMP:-ON}"
COLMAPKIT_VCPKG_ROOT="${COLMAPKIT_VCPKG_ROOT:-${VCPKG_ROOT:-}}"
COLMAPKIT_CMAKE_TOOLCHAIN_FILE="${COLMAPKIT_CMAKE_TOOLCHAIN_FILE:-${CMAKE_TOOLCHAIN_FILE:-}}"
COLMAPKIT_CMAKE_MAKE_PROGRAM="${COLMAPKIT_CMAKE_MAKE_PROGRAM:-${CMAKE_MAKE_PROGRAM:-}}"
COLMAPKIT_MACOS_VCPKG_INSTALLED_DIR="${COLMAPKIT_MACOS_VCPKG_INSTALLED_DIR:-"$BUILD_ROOT/vcpkg-installed-macos"}"
COLMAPKIT_IOS_VCPKG_INSTALLED_DIR="${COLMAPKIT_IOS_VCPKG_INSTALLED_DIR:-"$BUILD_ROOT/vcpkg-installed-ios"}"
LIBOMP_ROOT="${LIBOMP_ROOT:-"$BUILD_ROOT/libomp-macos$MACOS_DEPLOYMENT_TARGET"}"
VCPKG_REGISTRIES_CACHE="${X_VCPKG_REGISTRIES_CACHE:-"$BUILD_ROOT/vcpkg-registries-cache"}"
VCPKG_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-"$BUILD_ROOT/vcpkg-binary-cache"}"
REQUIRED_ENTRY_POINTS=(
  ColmapKitVersion
  ColmapKitInitialize
  ColmapKitRunSparseReconstruction
  ColmapKitStartSparseReconstruction
  ColmapKitCancelSparseReconstruction
  ColmapKitWaitSparseReconstruction
  ColmapKitReleaseSparseReconstructionJob
  ColmapKitRunPointFiltering
  ColmapKitRunModelCropping
  ColmapKitRunModelConversion
  ColmapKitGetABIVersionV2
  ColmapKitGetReleaseVersionV2
  ColmapKitGetEngineBuildIdentityV2
  ColmapKitRunTrackedPoseReconstructionV2
  ColmapKitStartTrackedPoseReconstructionV2
  ColmapKitCancelTrackedPoseReconstructionV2
  ColmapKitWaitTrackedPoseReconstructionV2
  ColmapKitReleaseTrackedPoseReconstructionJobV2
  ColmapKitRunRGBGaussianPriorV2
  ColmapKitStartRGBGaussianPriorV2
  ColmapKitCancelRGBGaussianPriorV2
  ColmapKitWaitRGBGaussianPriorV2
  ColmapKitReleaseRGBGaussianPriorJobV2
)

if [[ -z "$COLMAPKIT_CMAKE_TOOLCHAIN_FILE" && -n "$COLMAPKIT_VCPKG_ROOT" ]]; then
  COLMAPKIT_CMAKE_TOOLCHAIN_FILE="$COLMAPKIT_VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
fi
if [[ -z "$COLMAPKIT_CMAKE_MAKE_PROGRAM" && -n "$COLMAPKIT_VCPKG_ROOT" ]]; then
  COLMAPKIT_CMAKE_MAKE_PROGRAM="$(
    find "$COLMAPKIT_VCPKG_ROOT/downloads/tools" -type f -name ninja -print -quit 2>/dev/null || true
  )"
fi
if [[ ! -f "$COLMAPKIT_CMAKE_TOOLCHAIN_FILE" ]]; then
  echo "error: Set COLMAPKIT_VCPKG_ROOT or COLMAPKIT_CMAKE_TOOLCHAIN_FILE to a usable vcpkg checkout." >&2
  exit 1
fi

mkdir -p "$BUILD_ROOT" "$DIST_ROOT" "$AUDIT_ROOT" "$VCPKG_REGISTRIES_CACHE" "$VCPKG_BINARY_CACHE"

if [[ "$COLMAPKIT_BUILD_MACOS" == "ON" ]]; then
  if [[ "$COLMAPKIT_BUILD_LIBOMP" == "ON" || ! -f "$LIBOMP_ROOT/lib/libomp.dylib" ]]; then
    env \
      MACOS_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
      LIBOMP_BUILD_ROOT="$BUILD_ROOT/libomp-build" \
      LIBOMP_INSTALL_PREFIX="$LIBOMP_ROOT" \
      COLMAPKIT_CMAKE_MAKE_PROGRAM="$COLMAPKIT_CMAKE_MAKE_PROGRAM" \
      bash "$ROOT_DIR/scripts/build_libomp_macos.sh"
  fi

  env \
    COLMAPKIT_BUILD_ROOT="$BUILD_ROOT/macos" \
    COLMAPKIT_DIST_ROOT="$MACOS_PACKAGE_ROOT" \
    MACOS_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
    COLMAPKIT_CMAKE_TOOLCHAIN_FILE="$COLMAPKIT_CMAKE_TOOLCHAIN_FILE" \
    COLMAPKIT_CMAKE_MAKE_PROGRAM="$COLMAPKIT_CMAKE_MAKE_PROGRAM" \
    COLMAPKIT_VCPKG_TARGET_TRIPLET=arm64-osx-release-macos15 \
    COLMAPKIT_VCPKG_INSTALLED_DIR="$COLMAPKIT_MACOS_VCPKG_INSTALLED_DIR" \
    COLMAPKIT_VCPKG_OVERLAY_TRIPLETS="$ROOT_DIR/cmake/vcpkg-triplets" \
    COLMAPKIT_IGNORE_PREFIXES='/opt/homebrew;/usr/local;/opt/anaconda3' \
    LIBOMP_ROOT="$LIBOMP_ROOT" \
    X_VCPKG_REGISTRIES_CACHE="$VCPKG_REGISTRIES_CACHE" \
    VCPKG_DEFAULT_BINARY_CACHE="$VCPKG_BINARY_CACHE" \
    bash "$ROOT_DIR/scripts/build_colmapkit_xcframework.sh"
fi

if [[ "$COLMAPKIT_BUILD_IOS" == "ON" ]]; then
  env \
    COLMAPKIT_IOS_BUILD=ON \
    COLMAPKIT_IOS_USE_VCPKG=ON \
    COLMAPKIT_IOS_FAIL_ON_BLOCKER=ON \
    COLMAPKIT_IOS_BUILD_ROOT="$BUILD_ROOT/ios" \
    COLMAPKIT_IOS_DIST_ROOT="$IOS_PROBE_ROOT" \
    COLMAPKIT_IOS_PACKAGE_ROOT="$IOS_PACKAGE_ROOT" \
    IOS_DEPLOYMENT_TARGET="$IOS_DEPLOYMENT_TARGET" \
    COLMAPKIT_VCPKG_ROOT="$COLMAPKIT_VCPKG_ROOT" \
    COLMAPKIT_CMAKE_TOOLCHAIN_FILE="$COLMAPKIT_CMAKE_TOOLCHAIN_FILE" \
    COLMAPKIT_CMAKE_MAKE_PROGRAM="$COLMAPKIT_CMAKE_MAKE_PROGRAM" \
    COLMAPKIT_VCPKG_INSTALLED_DIR="$COLMAPKIT_IOS_VCPKG_INSTALLED_DIR" \
    X_VCPKG_REGISTRIES_CACHE="$VCPKG_REGISTRIES_CACHE" \
    VCPKG_DEFAULT_BINARY_CACHE="$VCPKG_BINARY_CACHE" \
    bash "$ROOT_DIR/scripts/probe_colmapkit_ios.sh"
fi

MACOS_FRAMEWORK="$MACOS_PACKAGE_ROOT/ColmapKit.xcframework/macos-arm64/ColmapKit.framework"
IOS_FRAMEWORK="$IOS_PACKAGE_ROOT/ColmapKit.xcframework/ios-arm64/ColmapKit.framework"
IOS_SIMULATOR_FRAMEWORK="$IOS_PACKAGE_ROOT/ColmapKit.xcframework/ios-arm64-simulator/ColmapKit.framework"

for framework in "$MACOS_FRAMEWORK" "$IOS_FRAMEWORK" "$IOS_SIMULATOR_FRAMEWORK"; do
  if [[ ! -d "$framework" ]]; then
    echo "error: Missing framework input: $framework" >&2
    exit 1
  fi
done

framework_header() {
  local framework="$1"
  find "$framework" -type f -path '*/Headers/colmapkit.h' -print -quit
}

framework_modulemap() {
  local framework="$1"
  find "$framework" -type f -path '*/Modules/module.modulemap' -print -quit
}

MACOS_HEADER="$(framework_header "$MACOS_FRAMEWORK")"
IOS_HEADER="$(framework_header "$IOS_FRAMEWORK")"
IOS_SIMULATOR_HEADER="$(framework_header "$IOS_SIMULATOR_FRAMEWORK")"
MACOS_MODULEMAP="$(framework_modulemap "$MACOS_FRAMEWORK")"
IOS_MODULEMAP="$(framework_modulemap "$IOS_FRAMEWORK")"
IOS_SIMULATOR_MODULEMAP="$(framework_modulemap "$IOS_SIMULATOR_FRAMEWORK")"

cmp "$MACOS_HEADER" "$IOS_HEADER"
cmp "$MACOS_HEADER" "$IOS_SIMULATOR_HEADER"
cmp "$MACOS_MODULEMAP" "$IOS_MODULEMAP"
cmp "$MACOS_MODULEMAP" "$IOS_SIMULATOR_MODULEMAP"
for header in "$MACOS_HEADER" "$IOS_HEADER" "$IOS_SIMULATOR_HEADER"; do
  for entry_point in "${REQUIRED_ENTRY_POINTS[@]}"; do
    grep -Fq "$entry_point" "$header"
  done
done
for modulemap in "$MACOS_MODULEMAP" "$IOS_MODULEMAP" "$IOS_SIMULATOR_MODULEMAP"; do
  grep -Fq 'umbrella header "colmapkit.h"' "$modulemap"
done
{
  printf 'Headers are byte-identical across all slices.\n'
  printf 'Module maps are byte-identical across all slices.\n'
  printf 'Every public header declares:\n'
  printf '  %s\n' "${REQUIRED_ENTRY_POINTS[@]}"
  printf 'Every module map exposes umbrella header "colmapkit.h".\n'
} > "$AUDIT_ROOT/public-interface.txt"

rm -rf "$XCFRAMEWORK_PATH"
xcodebuild -create-xcframework \
  -framework "$MACOS_FRAMEWORK" \
  -framework "$IOS_FRAMEWORK" \
  -framework "$IOS_SIMULATOR_FRAMEWORK" \
  -output "$XCFRAMEWORK_PATH"

find "$XCFRAMEWORK_PATH" -type f -name '*.dylib' -exec codesign --force --sign - {} \;
find "$XCFRAMEWORK_PATH" -type d -name 'ColmapKit.framework' -exec codesign --force --deep --sign - {} \;
codesign --force --sign - "$XCFRAMEWORK_PATH"

plutil -lint "$XCFRAMEWORK_PATH/Info.plist" > "$AUDIT_ROOT/xcframework-plutil-lint.txt"
plutil -p "$XCFRAMEWORK_PATH/Info.plist" > "$AUDIT_ROOT/xcframework-plutil.txt"
if [[ "$(find "$XCFRAMEWORK_PATH" -type d -name ColmapKit.framework | wc -l | tr -d ' ')" != "3" ]]; then
  echo "error: Combined XCFramework does not contain exactly three framework slices." >&2
  exit 1
fi

audit_framework() {
  local label="$1"
  local framework="$2"
  local expected_platform="$3"
  local expected_minos="$4"
  local binary
  binary="$(find "$framework" -type f \( -path '*/Versions/A/ColmapKit' -o -path '*/ColmapKit.framework/ColmapKit' \) -print -quit)"
  local info_plist
  info_plist="$(find "$framework" -type f -name Info.plist -print -quit)"
  local audit_path="$AUDIT_ROOT/$label.txt"
  local linkage_output
  linkage_output="$(otool -L "$binary")"

  if [[ -z "$binary" || -z "$info_plist" ]]; then
    echo "error: $label framework is incomplete." >&2
    exit 1
  fi

  {
    printf '# %s\n\n' "$label"
    printf '## plist\n'
    plutil -lint "$info_plist"
    plutil -p "$info_plist"
    printf '\n## file\n'
    file "$binary"
    printf '\n## vtool\n'
    xcrun vtool -show-build "$binary"
    printf '\n## otool -L\n'
    printf '%s\n' "$linkage_output"
    printf '\n## exported ColmapKit ABI\n'
    xcrun nm -gU "$binary" | awk '$3 ~ /^_ColmapKit/ { print $3 }' | sort
    printf '\n## codesign\n'
    codesign --display --verbose=4 "$framework"
    codesign --verify --deep --strict --verbose=2 "$framework"
  } > "$audit_path" 2>&1

  if ! grep -Fq "platform $expected_platform" "$audit_path" ||
     ! grep -Fq "minos $expected_minos" "$audit_path"; then
    echo "error: $label has an unexpected platform or deployment target." >&2
    exit 1
  fi
  if grep -E '^[[:space:]].*(/opt/homebrew|/usr/local|/opt/anaconda3|/private/tmp|/Users/|MacOSX)' <<< "$linkage_output" >/dev/null; then
    echo "error: $label contains a host or SDK path in its load commands." >&2
    exit 1
  fi
  if [[ "$expected_platform" != "MACOS" ]] &&
     grep -E '^[[:space:]].*(IOKit\.framework|ColorSync\.framework|\.framework/Versions/)' <<< "$linkage_output" >/dev/null; then
    echo "error: $label contains a macOS-only dependency." >&2
    exit 1
  fi

  xcrun nm -gU "$binary" | awk '$3 ~ /^_ColmapKit/ { print $3 }' | sort > "$AUDIT_ROOT/$label-symbols.txt"
  local entry_point
  for entry_point in "${REQUIRED_ENTRY_POINTS[@]}"; do
    if ! grep -Fxq "_$entry_point" "$AUDIT_ROOT/$label-symbols.txt"; then
      echo "error: $label is missing exported symbol $entry_point." >&2
      exit 1
    fi
  done
}

audit_framework macos-arm64 "$XCFRAMEWORK_PATH/macos-arm64/ColmapKit.framework" MACOS "$MACOS_DEPLOYMENT_TARGET"
audit_framework ios-arm64 "$XCFRAMEWORK_PATH/ios-arm64/ColmapKit.framework" IOS "$IOS_DEPLOYMENT_TARGET"
audit_framework ios-arm64-simulator "$XCFRAMEWORK_PATH/ios-arm64-simulator/ColmapKit.framework" IOSSIMULATOR "$IOS_DEPLOYMENT_TARGET"

cmp "$AUDIT_ROOT/macos-arm64-symbols.txt" "$AUDIT_ROOT/ios-arm64-symbols.txt"
cmp "$AUDIT_ROOT/macos-arm64-symbols.txt" "$AUDIT_ROOT/ios-arm64-simulator-symbols.txt"
codesign --verify --deep --strict --verbose=2 "$XCFRAMEWORK_PATH" > "$AUDIT_ROOT/xcframework-codesign.txt" 2>&1

SWIFT_LINK_ROOT="$AUDIT_ROOT/swift-link"
SWIFT_SOURCE="$SWIFT_LINK_ROOT/main.swift"
mkdir -p "$SWIFT_LINK_ROOT"
cat > "$SWIFT_SOURCE" <<'SWIFT'
import ColmapKit

_ = ColmapKitRunSparseReconstruction
_ = ColmapKitStartSparseReconstruction
_ = ColmapKitCancelSparseReconstruction
_ = ColmapKitWaitSparseReconstruction
_ = ColmapKitReleaseSparseReconstructionJob
_ = ColmapKitRunPointFiltering
_ = ColmapKitRunModelCropping
_ = ColmapKitRunModelConversion
_ = ColmapKitRunTrackedPoseReconstructionV2
_ = ColmapKitStartTrackedPoseReconstructionV2
_ = ColmapKitCancelTrackedPoseReconstructionV2
_ = ColmapKitWaitTrackedPoseReconstructionV2
_ = ColmapKitReleaseTrackedPoseReconstructionJobV2
_ = ColmapKitRunRGBGaussianPriorV2
_ = ColmapKitStartRGBGaussianPriorV2
_ = ColmapKitCancelRGBGaussianPriorV2
_ = ColmapKitWaitRGBGaussianPriorV2
_ = ColmapKitReleaseRGBGaussianPriorJobV2
precondition(ColmapKitGetABIVersionV2() == 2)
precondition(!String(cString: ColmapKitGetReleaseVersionV2()).isEmpty)
precondition(!String(cString: ColmapKitGetEngineBuildIdentityV2()).isEmpty)
precondition(!String(cString: ColmapKitVersion()).isEmpty)
SWIFT

swift_link_slice() {
  local label="$1"
  local sdk_name="$2"
  local target="$3"
  local framework_search_path="$4"
  local output="$SWIFT_LINK_ROOT/$label"
  local module_cache="$SWIFT_LINK_ROOT/$label-module-cache"
  local log="$SWIFT_LINK_ROOT/$label.log"
  local sdk_path
  sdk_path="$(xcrun --sdk "$sdk_name" --show-sdk-path)"
  mkdir -p "$module_cache"
  xcrun --sdk "$sdk_name" swiftc \
    -module-cache-path "$module_cache" \
    -Xcc "-fmodules-cache-path=$module_cache" \
    -sdk "$sdk_path" \
    -target "$target" \
    -F "$framework_search_path" \
    "$SWIFT_SOURCE" \
    -framework ColmapKit \
    -o "$output" > "$log" 2>&1
  {
    xcrun vtool -show-build "$output"
    otool -L "$output"
  } >> "$log" 2>&1
}

swift_link_slice \
  macos-arm64 \
  macosx \
  "arm64-apple-macos$MACOS_DEPLOYMENT_TARGET" \
  "$XCFRAMEWORK_PATH/macos-arm64"
swift_link_slice \
  ios-arm64 \
  iphoneos \
  "arm64-apple-ios$IOS_DEPLOYMENT_TARGET" \
  "$XCFRAMEWORK_PATH/ios-arm64"
swift_link_slice \
  ios-arm64-simulator \
  iphonesimulator \
  "arm64-apple-ios$IOS_DEPLOYMENT_TARGET-simulator" \
  "$XCFRAMEWORK_PATH/ios-arm64-simulator"

rm -f "$ZIP_PATH"
ditto -c -k --sequesterRsrc --keepParent "$XCFRAMEWORK_PATH" "$ZIP_PATH"
shasum -a 256 "$ZIP_PATH" > "$DIST_ROOT/ColmapKit.xcframework.zip.sha256"
swift package compute-checksum "$ZIP_PATH" > "$DIST_ROOT/ColmapKit.xcframework.zip.swiftpm-checksum"

SOURCE_COMMIT="$(git -C "$ROOT_DIR" rev-parse HEAD)"
SOURCE_PATCH_SHA256="$(git -C "$ROOT_DIR" diff --binary HEAD | shasum -a 256 | awk '{ print $1 }')"
FRAMEWORK_SIZE_BYTES="$(du -sk "$XCFRAMEWORK_PATH" | awk '{ print $1 * 1024 }')"
ZIP_SIZE_BYTES="$(stat -f '%z' "$ZIP_PATH")"
ZIP_SHA256="$(awk '{ print $1 }' "$DIST_ROOT/ColmapKit.xcframework.zip.sha256")"
SWIFTPM_CHECKSUM="$(tr -d '\n' < "$DIST_ROOT/ColmapKit.xcframework.zip.swiftpm-checksum")"

cat > "$DIST_ROOT/artifact-summary.txt" <<SUMMARY
source_commit=$SOURCE_COMMIT
source_revision=${COLMAPKIT_SOURCE_REVISION:-$SOURCE_COMMIT}
source_patch_sha256=$SOURCE_PATCH_SHA256
xcframework=$XCFRAMEWORK_PATH
framework_size_bytes=$FRAMEWORK_SIZE_BYTES
zip=$ZIP_PATH
zip_size_bytes=$ZIP_SIZE_BYTES
zip_sha256=$ZIP_SHA256
swiftpm_checksum=$SWIFTPM_CHECKSUM
slices=macos-arm64,ios-arm64,ios-arm64-simulator
macos_deployment_target=$MACOS_DEPLOYMENT_TARGET
ios_deployment_target=$IOS_DEPLOYMENT_TARGET
SUMMARY

echo "Created $XCFRAMEWORK_PATH"
echo "Created $ZIP_PATH"
echo "Wrote audits to $AUDIT_ROOT"
echo "Wrote release metadata to $DIST_ROOT/artifact-summary.txt"
