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
SIFT_METAL_ENABLED="${SIFT_METAL_ENABLED:-OFF}"
COLMAPKIT_RELEASE_VERSION="${COLMAPKIT_RELEASE_VERSION:-0.3.0-dev}"
COLMAPKIT_FRAMEWORK_VERSION="${COLMAPKIT_FRAMEWORK_VERSION:-0.3.0}"
COLMAPKIT_FRAMEWORK_BUILD_VERSION="${COLMAPKIT_FRAMEWORK_BUILD_VERSION:-1}"
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
SOURCE_COMMIT="$(git -C "$ROOT_DIR" rev-parse HEAD)"
COLMAPKIT_SOURCE_REVISION="${COLMAPKIT_SOURCE_REVISION:-$SOURCE_COMMIT}"
COLMAPKIT_REQUIRE_CLEAN_SOURCE="${COLMAPKIT_REQUIRE_CLEAN_SOURCE:-OFF}"
SOURCE_PATCH_SHA256="$(git -C "$ROOT_DIR" diff --binary HEAD | shasum -a 256 | awk '{ print $1 }')"
EMPTY_PATCH_SHA256="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
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
  ColmapKitCreateFrameFeatureExtractorV1
  ColmapKitStartFrameFeatureExtractionV1
  ColmapKitCancelFrameFeatureExtractionV1
  ColmapKitWaitFrameFeatureExtractionV1
  ColmapKitReleaseFrameFeatureJobV1
  ColmapKitReleaseFrameFeatureExtractorV1
  ColmapKitValidateFrameFeatureArtifactV1
  ColmapKitStartFrameFeatureImportV1
  ColmapKitCancelFrameFeatureImportV1
  ColmapKitWaitFrameFeatureImportV1
  ColmapKitReleaseFrameFeatureImportJobV1
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
if [[ "$COLMAPKIT_SOURCE_REVISION" != "$SOURCE_COMMIT" ]]; then
  echo "error: COLMAPKIT_SOURCE_REVISION must equal the full source HEAD ($SOURCE_COMMIT)." >&2
  exit 1
fi
if [[ "$COLMAPKIT_REQUIRE_CLEAN_SOURCE" == "ON" &&
      "$SOURCE_PATCH_SHA256" != "$EMPTY_PATCH_SHA256" ]]; then
  echo "error: Immutable package candidate requires a clean source checkout." >&2
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
    SIFT_METAL_ENABLED="$SIFT_METAL_ENABLED" \
    COLMAPKIT_SOURCE_REVISION="$COLMAPKIT_SOURCE_REVISION" \
    COLMAPKIT_RELEASE_VERSION="$COLMAPKIT_RELEASE_VERSION" \
    COLMAPKIT_FRAMEWORK_VERSION="$COLMAPKIT_FRAMEWORK_VERSION" \
    COLMAPKIT_FRAMEWORK_BUILD_VERSION="$COLMAPKIT_FRAMEWORK_BUILD_VERSION" \
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
    SIFT_METAL_ENABLED="$SIFT_METAL_ENABLED" \
    COLMAPKIT_SOURCE_REVISION="$COLMAPKIT_SOURCE_REVISION" \
    COLMAPKIT_RELEASE_VERSION="$COLMAPKIT_RELEASE_VERSION" \
    COLMAPKIT_FRAMEWORK_VERSION="$COLMAPKIT_FRAMEWORK_VERSION" \
    COLMAPKIT_FRAMEWORK_BUILD_VERSION="$COLMAPKIT_FRAMEWORK_BUILD_VERSION" \
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

NOTICE_NAME="ColmapKit-THIRD-PARTY-NOTICES.txt"
NOTICE_PATH="$AUDIT_ROOT/$NOTICE_NAME"
NOTICE_INPUTS_RAW="$AUDIT_ROOT/license-inputs.raw.tsv"
NOTICE_INPUTS_SORTED="$AUDIT_ROOT/license-inputs.sorted.tsv"
NOTICE_INPUTS="$AUDIT_ROOT/license-inputs.txt"
: > "$NOTICE_INPUTS_RAW"

add_notice_input() {
  local label="$1"
  local path="$2"
  if [[ ! -f "$path" ]]; then
    echo "error: Missing required license input: $label ($path)" >&2
    exit 1
  fi
  printf '%s\t%s\t%s\n' \
    "$label" \
    "$(shasum -a 256 "$path" | awk '{ print $1 }')" \
    "$path" >> "$NOTICE_INPUTS_RAW"
}

add_notice_input "COLMAP/COPYING.txt" "$ROOT_DIR/COPYING.txt"
while IFS= read -r license_path; do
  add_notice_input \
    "COLMAP/${license_path#"$ROOT_DIR/"}" \
    "$license_path"
done < <(find "$ROOT_DIR/src/thirdparty" -maxdepth 2 -type f \
  \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' \) | sort)

for dependency in faiss poselib; do
  dependency_license="$(find "$BUILD_ROOT/macos" -type f \
    -path "*/_deps/$dependency-src/LICENSE" -print -quit)"
  add_notice_input "fetched/$dependency/LICENSE" "$dependency_license"
done

OPENMP_LICENSE="$(find "$BUILD_ROOT/libomp-build/src" -type f \
  -path '*/openmp/LICENSE.TXT' -print -quit)"
add_notice_input "runtime/openmp/LICENSE.TXT" "$OPENMP_LICENSE"

for installed_root in \
  "$COLMAPKIT_MACOS_VCPKG_INSTALLED_DIR" \
  "$COLMAPKIT_IOS_VCPKG_INSTALLED_DIR"; do
  while IFS= read -r copyright_path; do
    package_name="$(basename "$(dirname "$copyright_path")")"
    add_notice_input "vcpkg/$package_name/copyright" "$copyright_path"
  done < <(find "$installed_root" -type f -path '*/share/*/copyright' | sort)
done

LC_ALL=C sort -t $'\t' -k1,1 -k2,2 "$NOTICE_INPUTS_RAW" > "$NOTICE_INPUTS_SORTED"
: > "$NOTICE_INPUTS"
cat > "$NOTICE_PATH" <<'NOTICE'
ColmapKit third-party notices
================================

This file collects the license texts shipped with ColmapKit and its packaged
binary dependencies. The section labels are deterministic provenance labels;
they are not host filesystem paths.
NOTICE

previous_label=""
previous_sha=""
while IFS=$'\t' read -r label sha256 license_path; do
  if [[ "$label" == "$previous_label" ]]; then
    if [[ "$sha256" != "$previous_sha" ]]; then
      echo "error: License text differs across slices for $label." >&2
      exit 1
    fi
    continue
  fi
  printf '%s  %s\n' "$sha256" "$label" >> "$NOTICE_INPUTS"
  {
    printf '\n\n------------------------------------------------------------------------\n'
    printf '%s\n' "$label"
    printf '%s\n' '------------------------------------------------------------------------'
    cat "$license_path"
  } >> "$NOTICE_PATH"
  previous_label="$label"
  previous_sha="$sha256"
done < "$NOTICE_INPUTS_SORTED"
rm -f "$NOTICE_INPUTS_RAW" "$NOTICE_INPUTS_SORTED"

install_notice() {
  local framework="$1"
  if [[ -d "$framework/Versions/A" ]]; then
    local resources="$framework/Versions/A/Resources"
    mkdir -p "$resources"
    cp "$NOTICE_PATH" "$resources/$NOTICE_NAME"
  else
    cp "$NOTICE_PATH" "$framework/$NOTICE_NAME"
  fi
}

install_notice "$MACOS_FRAMEWORK"
install_notice "$IOS_FRAMEWORK"
install_notice "$IOS_SIMULATOR_FRAMEWORK"

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
  if [[ "$(plutil -extract CFBundleName raw "$info_plist")" != "ColmapKit" ||
        "$(plutil -extract CFBundleShortVersionString raw "$info_plist")" != "$COLMAPKIT_FRAMEWORK_VERSION" ||
        "$(plutil -extract CFBundleVersion raw "$info_plist")" != "$COLMAPKIT_FRAMEWORK_BUILD_VERSION" ]]; then
    echo "error: $label has invalid framework identity metadata." >&2
    exit 1
  fi

  local metallib
  metallib="$(find "$framework" -type f -name sift.metallib -print -quit)"
  if [[ "$SIFT_METAL_ENABLED" == "ON" && -z "$metallib" ]]; then
    echo "error: $label is missing the packaged SiftMetal library." >&2
    exit 1
  fi

  local notices
  notices="$(find "$framework" -type f -name "$NOTICE_NAME" -print -quit)"
  if [[ -z "$notices" ]]; then
    echo "error: $label is missing $NOTICE_NAME." >&2
    exit 1
  fi
  if ! cmp -s "$NOTICE_PATH" "$notices"; then
    echo "error: $label contains a noncanonical third-party notice." >&2
    exit 1
  fi
  if [[ "$SIFT_METAL_ENABLED" != "ON" && -n "$metallib" ]]; then
    echo "error: $label unexpectedly contains a SiftMetal library." >&2
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
    if [[ -n "$metallib" ]]; then
      printf '\n## SiftMetal library\n'
      file "$metallib"
      shasum -a 256 "$metallib"
    fi
    printf '\n## license notices\n'
    shasum -a 256 "$notices"
  } > "$audit_path" 2>&1

  if ! grep -Fq "platform $expected_platform" "$audit_path" ||
     ! grep -Fq "minos $expected_minos" "$audit_path"; then
    echo "error: $label has an unexpected platform or deployment target." >&2
    exit 1
  fi
  if ! grep -Fq 'Info.plist entries=' "$audit_path"; then
    echo "error: $label code signature does not bind its Info.plist." >&2
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
cat > "$SWIFT_SOURCE" <<SWIFT
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
_ = ColmapKitCreateFrameFeatureExtractorV1
_ = ColmapKitStartFrameFeatureExtractionV1
_ = ColmapKitCancelFrameFeatureExtractionV1
_ = ColmapKitWaitFrameFeatureExtractionV1
_ = ColmapKitReleaseFrameFeatureJobV1
_ = ColmapKitReleaseFrameFeatureExtractorV1
_ = ColmapKitValidateFrameFeatureArtifactV1
_ = ColmapKitStartFrameFeatureImportV1
_ = ColmapKitCancelFrameFeatureImportV1
_ = ColmapKitWaitFrameFeatureImportV1
_ = ColmapKitReleaseFrameFeatureImportJobV1
precondition(ColmapKitGetABIVersionV2() == 2)
precondition(String(cString: ColmapKitGetReleaseVersionV2()) == "$COLMAPKIT_RELEASE_VERSION")
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
DYLD_FRAMEWORK_PATH="$XCFRAMEWORK_PATH/macos-arm64" \
  "$SWIFT_LINK_ROOT/macos-arm64" > "$SWIFT_LINK_ROOT/macos-arm64-runtime.log" 2>&1

SWIFT_PACKAGE_ROOT="$AUDIT_ROOT/swift-package-consumer"
rm -rf "$SWIFT_PACKAGE_ROOT"
mkdir -p "$SWIFT_PACKAGE_ROOT/Sources/ColmapKitConsumer"
ln -s ../../ColmapKit.xcframework "$SWIFT_PACKAGE_ROOT/ColmapKit.xcframework"
cat > "$SWIFT_PACKAGE_ROOT/Package.swift" <<'SWIFT_PACKAGE'
// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "ColmapKitConsumerAudit",
    platforms: [.macOS(.v15), .iOS(.v18)],
    products: [
        .library(name: "ColmapKitConsumer", targets: ["ColmapKitConsumer"]),
    ],
    targets: [
        .binaryTarget(name: "ColmapKit", path: "ColmapKit.xcframework"),
        .target(name: "ColmapKitConsumer", dependencies: ["ColmapKit"]),
    ]
)
SWIFT_PACKAGE
cat > "$SWIFT_PACKAGE_ROOT/Sources/ColmapKitConsumer/ColmapKitConsumer.swift" <<'SWIFT_CONSUMER'
import ColmapKit

public enum ColmapKitConsumerAudit {
  public static func verifyPublicSurface() {
    _ = ColmapKitRunSparseReconstruction
    _ = ColmapKitStartSparseReconstruction
    _ = ColmapKitRunTrackedPoseReconstructionV2
    _ = ColmapKitRunRGBGaussianPriorV2
    _ = ColmapKitCreateFrameFeatureExtractorV1
    _ = ColmapKitStartFrameFeatureExtractionV1
    _ = ColmapKitCancelFrameFeatureExtractionV1
    _ = ColmapKitWaitFrameFeatureExtractionV1
    _ = ColmapKitReleaseFrameFeatureJobV1
    _ = ColmapKitReleaseFrameFeatureExtractorV1
    _ = ColmapKitValidateFrameFeatureArtifactV1
    _ = ColmapKitStartFrameFeatureImportV1
    _ = ColmapKitCancelFrameFeatureImportV1
    _ = ColmapKitWaitFrameFeatureImportV1
    _ = ColmapKitReleaseFrameFeatureImportJobV1
    precondition(ColmapKitGetABIVersionV2() == 2)
  }
}
SWIFT_CONSUMER

swift_package_build() {
  local label="$1"
  local triple="$2"
  local sdk="$3"
  swift build \
    --package-path "$SWIFT_PACKAGE_ROOT" \
    --scratch-path "$SWIFT_PACKAGE_ROOT/.build-$label" \
    --triple "$triple" \
    --sdk "$sdk" > "$SWIFT_PACKAGE_ROOT/$label.log" 2>&1
}

swift_package_build \
  macos-arm64 \
  "arm64-apple-macosx$MACOS_DEPLOYMENT_TARGET" \
  "$(xcrun --sdk macosx --show-sdk-path)"
swift_package_build \
  ios-arm64 \
  "arm64-apple-ios$IOS_DEPLOYMENT_TARGET" \
  "$(xcrun --sdk iphoneos --show-sdk-path)"
swift_package_build \
  ios-arm64-simulator \
  "arm64-apple-ios$IOS_DEPLOYMENT_TARGET-simulator" \
  "$(xcrun --sdk iphonesimulator --show-sdk-path)"

rm -f "$ZIP_PATH"
ditto -c -k --sequesterRsrc --keepParent "$XCFRAMEWORK_PATH" "$ZIP_PATH"
shasum -a 256 "$ZIP_PATH" > "$DIST_ROOT/ColmapKit.xcframework.zip.sha256"
swift package compute-checksum "$ZIP_PATH" > "$DIST_ROOT/ColmapKit.xcframework.zip.swiftpm-checksum"

FRAMEWORK_SIZE_BYTES="$(du -sk "$XCFRAMEWORK_PATH" | awk '{ print $1 * 1024 }')"
ZIP_SIZE_BYTES="$(stat -f '%z' "$ZIP_PATH")"
ZIP_SHA256="$(awk '{ print $1 }' "$DIST_ROOT/ColmapKit.xcframework.zip.sha256")"
SWIFTPM_CHECKSUM="$(tr -d '\n' < "$DIST_ROOT/ColmapKit.xcframework.zip.swiftpm-checksum")"
HEADER_SHA256="$(shasum -a 256 "$MACOS_HEADER" | awk '{ print $1 }')"
MODULEMAP_SHA256="$(shasum -a 256 "$MACOS_MODULEMAP" | awk '{ print $1 }')"
NOTICE_SHA256="$(shasum -a 256 "$NOTICE_PATH" | awk '{ print $1 }')"
XCODE_VERSION="$(xcodebuild -version | tr '\n' ' ')"
MACOS_SDK_VERSION="$(xcrun --sdk macosx --show-sdk-version)"
IPHONEOS_SDK_VERSION="$(xcrun --sdk iphoneos --show-sdk-version)"
IPHONESIMULATOR_SDK_VERSION="$(xcrun --sdk iphonesimulator --show-sdk-version)"

{
  find "$XCFRAMEWORK_PATH" -type f -print0 | LC_ALL=C sort -z |
    while IFS= read -r -d '' artifact; do
      printf '%s  %s\n' \
        "$(shasum -a 256 "$artifact" | awk '{ print $1 }')" \
        "${artifact#"$DIST_ROOT/"}"
    done
  printf '%s  %s\n' "$ZIP_SHA256" "${ZIP_PATH#"$DIST_ROOT/"}"
} > "$AUDIT_ROOT/package-hashes.txt"

cat > "$DIST_ROOT/artifact-summary.txt" <<SUMMARY
source_commit=$SOURCE_COMMIT
source_revision=${COLMAPKIT_SOURCE_REVISION:-$SOURCE_COMMIT}
release_version=$COLMAPKIT_RELEASE_VERSION
framework_version=$COLMAPKIT_FRAMEWORK_VERSION
framework_build_version=$COLMAPKIT_FRAMEWORK_BUILD_VERSION
source_patch_sha256=$SOURCE_PATCH_SHA256
xcframework=$XCFRAMEWORK_PATH
framework_size_bytes=$FRAMEWORK_SIZE_BYTES
zip=$ZIP_PATH
zip_size_bytes=$ZIP_SIZE_BYTES
zip_sha256=$ZIP_SHA256
swiftpm_checksum=$SWIFTPM_CHECKSUM
public_header_sha256=$HEADER_SHA256
modulemap_sha256=$MODULEMAP_SHA256
third_party_notices_sha256=$NOTICE_SHA256
slices=macos-arm64,ios-arm64,ios-arm64-simulator
macos_deployment_target=$MACOS_DEPLOYMENT_TARGET
ios_deployment_target=$IOS_DEPLOYMENT_TARGET
sift_metal_enabled=$SIFT_METAL_ENABLED
xcode=$XCODE_VERSION
macos_sdk=$MACOS_SDK_VERSION
iphoneos_sdk=$IPHONEOS_SDK_VERSION
iphonesimulator_sdk=$IPHONESIMULATOR_SDK_VERSION
cmake=$(cmake --version | head -1)
ninja=$($COLMAPKIT_CMAKE_MAKE_PROGRAM --version)
vcpkg_root=$COLMAPKIT_VCPKG_ROOT
vcpkg_commit=$(git -C "$COLMAPKIT_VCPKG_ROOT" rev-parse HEAD)
SUMMARY

echo "Created $XCFRAMEWORK_PATH"
echo "Created $ZIP_PATH"
echo "Wrote audits to $AUDIT_ROOT"
echo "Wrote release metadata to $DIST_ROOT/artifact-summary.txt"
