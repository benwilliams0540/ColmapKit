#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${COLMAPKIT_IOS_BUILD_ROOT:-"$ROOT_DIR/build-colmapkit-ios-probe"}"
DIST_ROOT="${COLMAPKIT_IOS_DIST_ROOT:-"$ROOT_DIR/dist/colmapkit-ios-probe"}"
PACKAGE_ROOT="${COLMAPKIT_IOS_PACKAGE_ROOT:-"$ROOT_DIR/dist/colmapkit-ios"}"
XCFRAMEWORK_PATH="$PACKAGE_ROOT/ColmapKit.xcframework"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-18.0}"
COLMAPKIT_CLEAN="${COLMAPKIT_CLEAN:-ON}"
COLMAPKIT_IOS_STRICT_DEPS="${COLMAPKIT_IOS_STRICT_DEPS:-ON}"
COLMAPKIT_IOS_BUILD="${COLMAPKIT_IOS_BUILD:-ON}"
COLMAPKIT_IOS_PACKAGE="${COLMAPKIT_IOS_PACKAGE:-ON}"
COLMAPKIT_IOS_SMOKE_TEST="${COLMAPKIT_IOS_SMOKE_TEST:-ON}"
COLMAPKIT_IOS_FAIL_ON_BLOCKER="${COLMAPKIT_IOS_FAIL_ON_BLOCKER:-OFF}"
COLMAPKIT_IOS_USE_VCPKG="${COLMAPKIT_IOS_USE_VCPKG:-OFF}"
COLMAPKIT_VCPKG_ROOT="${COLMAPKIT_VCPKG_ROOT:-${VCPKG_ROOT:-}}"
COLMAPKIT_CMAKE_TOOLCHAIN_FILE="${COLMAPKIT_CMAKE_TOOLCHAIN_FILE:-${CMAKE_TOOLCHAIN_FILE:-}}"
COLMAPKIT_CMAKE_MAKE_PROGRAM="${COLMAPKIT_CMAKE_MAKE_PROGRAM:-${CMAKE_MAKE_PROGRAM:-}}"
COLMAPKIT_VCPKG_INSTALLED_DIR="${COLMAPKIT_VCPKG_INSTALLED_DIR:-${VCPKG_INSTALLED_DIR:-}}"
COLMAPKIT_VCPKG_OVERLAY_TRIPLETS="${COLMAPKIT_VCPKG_OVERLAY_TRIPLETS:-${VCPKG_OVERLAY_TRIPLETS:-}}"
COLMAPKIT_VCPKG_OVERLAY_PORTS="${COLMAPKIT_VCPKG_OVERLAY_PORTS:-${VCPKG_OVERLAY_PORTS:-}}"
COLMAPKIT_IOS_DEVICE_VCPKG_TARGET_TRIPLET="${COLMAPKIT_IOS_DEVICE_VCPKG_TARGET_TRIPLET:-arm64-ios-release}"
COLMAPKIT_IOS_SIMULATOR_VCPKG_TARGET_TRIPLET="${COLMAPKIT_IOS_SIMULATOR_VCPKG_TARGET_TRIPLET:-arm64-ios-simulator-release}"
COLMAPKIT_IOS_VCPKG_MANIFEST_NO_DEFAULT_FEATURES="${COLMAPKIT_IOS_VCPKG_MANIFEST_NO_DEFAULT_FEATURES:-ON}"

if [[ -z "$COLMAPKIT_CMAKE_TOOLCHAIN_FILE" && -n "$COLMAPKIT_VCPKG_ROOT" ]]; then
  COLMAPKIT_CMAKE_TOOLCHAIN_FILE="$COLMAPKIT_VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
fi
if [[ -z "$COLMAPKIT_CMAKE_MAKE_PROGRAM" && -n "$COLMAPKIT_VCPKG_ROOT" ]]; then
  COLMAPKIT_CMAKE_MAKE_PROGRAM="$(
    find "$COLMAPKIT_VCPKG_ROOT/downloads/tools" -type f -name ninja -print -quit 2>/dev/null || true
  )"
fi
if [[ -z "$COLMAPKIT_VCPKG_OVERLAY_TRIPLETS" && -d "$ROOT_DIR/cmake/vcpkg-triplets" ]]; then
  COLMAPKIT_VCPKG_OVERLAY_TRIPLETS="$ROOT_DIR/cmake/vcpkg-triplets"
fi
if [[ -z "$COLMAPKIT_VCPKG_OVERLAY_PORTS" && -d "$ROOT_DIR/cmake/vcpkg-ports" ]]; then
  COLMAPKIT_VCPKG_OVERLAY_PORTS="$ROOT_DIR/cmake/vcpkg-ports"
fi

mkdir -p "$DIST_ROOT"
SUMMARY_PATH="$DIST_ROOT/summary.md"

cat > "$SUMMARY_PATH" <<SUMMARY
# ColmapKit iOS Probe

Generated from:

\`\`\`text
$ROOT_DIR
\`\`\`

Settings:

- Generator: $GENERATOR
- iOS deployment target: $IOS_DEPLOYMENT_TARGET
- Strict dependency mode: $COLMAPKIT_IOS_STRICT_DEPS
- Build after configure: $COLMAPKIT_IOS_BUILD
- Package XCFramework: $COLMAPKIT_IOS_PACKAGE
- Compile and link smoke client: $COLMAPKIT_IOS_SMOKE_TEST
- XCFramework path: $XCFRAMEWORK_PATH
- vcpkg mode: $COLMAPKIT_IOS_USE_VCPKG
- vcpkg root: ${COLMAPKIT_VCPKG_ROOT:-unset}
- vcpkg toolchain: ${COLMAPKIT_CMAKE_TOOLCHAIN_FILE:-unset}
- CMake make program: ${COLMAPKIT_CMAKE_MAKE_PROGRAM:-default}
- vcpkg installed dir: ${COLMAPKIT_VCPKG_INSTALLED_DIR:-default}
- vcpkg overlay triplets: ${COLMAPKIT_VCPKG_OVERLAY_TRIPLETS:-unset}
- vcpkg overlay ports: ${COLMAPKIT_VCPKG_OVERLAY_PORTS:-unset}
- vcpkg default manifest features disabled: $COLMAPKIT_IOS_VCPKG_MANIFEST_NO_DEFAULT_FEATURES

SUMMARY

append_summary() {
  printf '%s\n' "$*" >> "$SUMMARY_PATH"
}

make_ignore_prefix_path() {
  local value="${CMAKE_IGNORE_PREFIX_PATH:-}"
  local prefix
  for prefix in /opt/homebrew /usr/local /opt/anaconda3; do
    if [[ -d "$prefix" ]]; then
      if [[ -n "$value" ]]; then
        value="$prefix;$value"
      else
        value="$prefix"
      fi
    fi
  done
  printf '%s' "$value"
}

run_slice() {
  local label="$1"
  local sdk_name="$2"
  local system_name="$3"
  local architectures="$4"
  local vcpkg_triplet="$5"

  local build_dir="$BUILD_ROOT/$label"
  local configure_log="$DIST_ROOT/$label-configure.log"
  local build_log="$DIST_ROOT/$label-build.log"
  local sdk_path
  sdk_path="$(xcrun --sdk "$sdk_name" --show-sdk-path)"
  local c_compiler
  local cxx_compiler
  c_compiler="${CMAKE_C_COMPILER:-"$(xcrun --sdk "$sdk_name" --find clang)"}"
  cxx_compiler="${CMAKE_CXX_COMPILER:-"$(xcrun --sdk "$sdk_name" --find clang++)"}"

  if [[ "$COLMAPKIT_CLEAN" == "ON" ]]; then
    cmake -E rm -rf "$build_dir"
  fi

  local configure_args=(
    cmake -S "$ROOT_DIR" -B "$build_dir" -G "$GENERATOR"
    -DCMAKE_C_COMPILER="$c_compiler"
    -DCMAKE_CXX_COMPILER="$cxx_compiler"
    -DCMAKE_OBJCXX_COMPILER="$cxx_compiler"
    -DCMAKE_SYSTEM_NAME="$system_name"
    -DCMAKE_OSX_SYSROOT="$sdk_path"
    -DCMAKE_OSX_ARCHITECTURES="$architectures"
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOYMENT_TARGET"
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    -DCMAKE_BUILD_TYPE=Release
    -DCOLMAPKIT_FRAMEWORK_ENABLED=ON
    -DGUI_ENABLED=OFF
    -DCUDA_ENABLED=OFF
    -DOPENGL_ENABLED=OFF
    -DMVS_ENABLED=OFF
    -DONNX_ENABLED=OFF
    -DCGAL_ENABLED=OFF
    -DDOWNLOAD_ENABLED=OFF
    -DTESTS_ENABLED=OFF
    -DOPENMP_ENABLED=OFF
    -DMETAL_ENABLED=ON
    -DSIFT_METAL_ENABLED=OFF
    -DBUILD_SHARED_LIBS=OFF
  )

  if [[ "$COLMAPKIT_IOS_USE_VCPKG" == "ON" ]]; then
    if [[ -z "$COLMAPKIT_CMAKE_TOOLCHAIN_FILE" || ! -f "$COLMAPKIT_CMAKE_TOOLCHAIN_FILE" ]]; then
      append_summary "- Configure status: skipped"
      append_summary "- Result: vcpkg mode requested, but no usable vcpkg toolchain file was found."
      append_summary ""
      return 1
    fi
    configure_args+=("-DCMAKE_TOOLCHAIN_FILE=$COLMAPKIT_CMAKE_TOOLCHAIN_FILE")
    configure_args+=("-DVCPKG_TARGET_TRIPLET=$vcpkg_triplet")
    configure_args+=("-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=$COLMAPKIT_IOS_VCPKG_MANIFEST_NO_DEFAULT_FEATURES")
    if [[ -n "$COLMAPKIT_CMAKE_MAKE_PROGRAM" ]]; then
      configure_args+=("-DCMAKE_MAKE_PROGRAM=$COLMAPKIT_CMAKE_MAKE_PROGRAM")
    fi
    if [[ -n "$COLMAPKIT_VCPKG_INSTALLED_DIR" ]]; then
      configure_args+=("-DVCPKG_INSTALLED_DIR=$COLMAPKIT_VCPKG_INSTALLED_DIR")
    fi
    if [[ -n "$COLMAPKIT_VCPKG_OVERLAY_TRIPLETS" ]]; then
      configure_args+=("-DVCPKG_OVERLAY_TRIPLETS=$COLMAPKIT_VCPKG_OVERLAY_TRIPLETS")
    fi
    if [[ -n "$COLMAPKIT_VCPKG_OVERLAY_PORTS" ]]; then
      configure_args+=("-DVCPKG_OVERLAY_PORTS=$COLMAPKIT_VCPKG_OVERLAY_PORTS")
    fi
  fi

  if [[ "$COLMAPKIT_IOS_STRICT_DEPS" == "ON" ]]; then
    local ignore_prefix_path
    ignore_prefix_path="$(make_ignore_prefix_path)"
    if [[ -n "$ignore_prefix_path" ]]; then
      configure_args+=("-DCMAKE_IGNORE_PREFIX_PATH=$ignore_prefix_path")
    fi
    configure_args+=("-DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE")
    configure_args+=("-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE")
  fi

  append_summary "## $label"
  append_summary ""
  append_summary "- SDK: $sdk_name"
  append_summary "- SDK path: $sdk_path"
  append_summary "- Architectures: $architectures"
  append_summary "- vcpkg triplet: ${vcpkg_triplet:-none}"
  append_summary "- Configure log: \`$configure_log\`"
  append_summary "- Build log: \`$build_log\`"

  printf 'Configuring %s...\n' "$label"
  set +e
  "${configure_args[@]}" > "$configure_log" 2>&1
  local configure_status=$?
  set -e

  append_summary "- Configure status: $configure_status"
  if [[ "$configure_status" -ne 0 ]]; then
    append_summary ""
    append_summary "Last configure log lines:"
    append_summary ""
    append_summary '```text'
    tail -40 "$configure_log" >> "$SUMMARY_PATH"
    append_summary '```'
    append_summary ""
    return 1
  fi

  if [[ "$COLMAPKIT_IOS_BUILD" != "ON" ]]; then
    append_summary "- Build status: skipped"
    append_summary ""
    return 0
  fi

  printf 'Building %s...\n' "$label"
  set +e
  cmake --build "$build_dir" --target ColmapKit > "$build_log" 2>&1
  local build_status=$?
  set -e

  append_summary "- Build status: $build_status"
  if [[ "$build_status" -ne 0 ]]; then
    append_summary ""
    append_summary "Last build log lines:"
    append_summary ""
    append_summary '```text'
    tail -40 "$build_log" >> "$SUMMARY_PATH"
    append_summary '```'
    append_summary ""
    return 1
  fi

  append_summary "- Result: ColmapKit framework target built"
  append_summary ""
  return 0
}

audit_framework() {
  local label="$1"
  local framework="$2"
  local expected_platform="$3"
  local binary="$framework/ColmapKit"

  if [[ ! -f "$binary" ||
        ! -f "$framework/Headers/colmapkit.h" ||
        ! -f "$framework/Modules/module.modulemap" ||
        ! -f "$framework/Info.plist" ]]; then
    append_summary "- $label audit: failed (framework bundle is incomplete)"
    return 1
  fi

  local file_output
  file_output="$(file "$binary")"
  if [[ "$file_output" != *"arm64"* ]]; then
    append_summary "- $label audit: failed (binary is not arm64)"
    return 1
  fi

  local lipo_output
  lipo_output="$(xcrun lipo -info "$binary")"
  if [[ "$lipo_output" != *"arm64"* ]]; then
    append_summary "- $label audit: failed (lipo did not report arm64)"
    return 1
  fi

  local build_output
  build_output="$(xcrun vtool -show-build "$binary")"
  if [[ "$build_output" != *"platform $expected_platform"* ||
        "$build_output" != *"minos $IOS_DEPLOYMENT_TARGET"* ]]; then
    append_summary "- $label audit: failed (unexpected platform or minimum OS)"
    return 1
  fi

  local linkage_output
  linkage_output="$(otool -L "$binary")"
  if grep -E '^[[:space:]].*(/opt/homebrew|/usr/local|/opt/anaconda3|/private/tmp|/Users/|MacOSX|\.framework/Versions/|IOKit\.framework)' <<< "$linkage_output" >/dev/null; then
    append_summary "- $label audit: failed (host or macOS-only dependency path found)"
    return 1
  fi

  local dependency_line
  while IFS= read -r dependency_line; do
    dependency_line="${dependency_line#"${dependency_line%%[![:space:]]*}"}"
    local dependency="${dependency_line%% *}"
    case "$dependency" in
      @rpath/ColmapKit.framework/ColmapKit | /System/Library/Frameworks/* | /usr/lib/*)
        ;;
      *)
        append_summary "- $label audit: failed (unexpected dependency $dependency)"
        return 1
        ;;
    esac
  done < <(tail -n +2 <<< "$linkage_output")

  local symbols_output
  symbols_output="$(xcrun nm -gU "$binary" | awk '$3 ~ /^_ColmapKit/ { print }')"
  local symbol
  for symbol in \
    ColmapKitVersion \
    ColmapKitInitialize \
    ColmapKitRunSparseReconstruction \
    ColmapKitRunPointFiltering \
    ColmapKitRunModelCropping \
    ColmapKitRunModelConversion \
    ColmapKitStartSparseReconstruction \
    ColmapKitCancelSparseReconstruction \
    ColmapKitWaitSparseReconstruction \
    ColmapKitReleaseSparseReconstructionJob; do
    if ! grep -Eq "[[:space:]]_${symbol}$" <<< "$symbols_output"; then
      append_summary "- $label audit: failed (missing exported symbol $symbol)"
      return 1
    fi
  done

  {
    printf '## %s\n\n' "$label"
    printf '%s\n\n' "$file_output"
    printf '%s\n\n' "$lipo_output"
    printf '%s\n\n' "$build_output"
    printf '%s\n\n' "$linkage_output"
    printf '%s\n' "$symbols_output"
  } >> "$PACKAGE_ROOT/framework-audit.txt"

  append_summary "- $label audit: passed"
  return 0
}

package_xcframework() {
  local device_framework="$BUILD_ROOT/ios-arm64/src/colmap/colmapkit/ColmapKit.framework"
  local simulator_framework="$BUILD_ROOT/ios-simulator-arm64/src/colmap/colmapkit/ColmapKit.framework"
  local package_log="$PACKAGE_ROOT/package.log"
  local plist_audit="$PACKAGE_ROOT/xcframework-info.txt"
  local smoke_root="$PACKAGE_ROOT/smoke"

  mkdir -p "$PACKAGE_ROOT"
  cmake -E rm -rf "$XCFRAMEWORK_PATH" "$smoke_root"
  : > "$PACKAGE_ROOT/framework-audit.txt"

  append_summary "## XCFramework package"
  append_summary ""
  append_summary "- Package log: \`$package_log\`"

  printf 'Packaging ColmapKit.xcframework...\n'
  set +e
  xcodebuild -create-xcframework \
    -framework "$device_framework" \
    -framework "$simulator_framework" \
    -output "$XCFRAMEWORK_PATH" > "$package_log" 2>&1
  local package_status=$?
  set -e
  append_summary "- Package status: $package_status"
  if [[ "$package_status" -ne 0 ]]; then
    append_summary ""
    append_summary "Last package log lines:"
    append_summary ""
    append_summary '```text'
    tail -40 "$package_log" >> "$SUMMARY_PATH"
    append_summary '```'
    append_summary ""
    return 1
  fi

  local packaged_device="$XCFRAMEWORK_PATH/ios-arm64/ColmapKit.framework"
  local packaged_simulator="$XCFRAMEWORK_PATH/ios-arm64-simulator/ColmapKit.framework"
  plutil -p "$XCFRAMEWORK_PATH/Info.plist" > "$plist_audit"
  if [[ "$(find "$XCFRAMEWORK_PATH" -type d -name ColmapKit.framework | wc -l | tr -d ' ')" != "2" ]] ||
     [[ "$(grep -c 'SupportedPlatform.*ios' "$plist_audit")" != "2" ]] ||
     [[ "$(grep -c 'SupportedPlatformVariant.*simulator' "$plist_audit")" != "1" ]]; then
    append_summary "- XCFramework metadata: failed"
    return 1
  fi
  append_summary "- XCFramework metadata: device and simulator variants present"

  if ! audit_framework "iPhoneOS arm64" "$packaged_device" IOS; then
    return 1
  fi
  if ! audit_framework "iPhoneSimulator arm64" "$packaged_simulator" IOSSIMULATOR; then
    return 1
  fi

  if [[ "$COLMAPKIT_IOS_SMOKE_TEST" == "ON" ]]; then
    mkdir -p "$smoke_root"
    local smoke_source="$smoke_root/main.swift"
    local smoke_binary="$smoke_root/colmapkit-smoke"
    local smoke_log="$smoke_root/build.log"
    local module_cache="$smoke_root/module-cache"
    mkdir -p "$module_cache"
    cat > "$smoke_source" <<'SWIFT'
import ColmapKit

_ = ColmapKitRunSparseReconstruction
_ = ColmapKitRunPointFiltering
_ = ColmapKitRunModelCropping
_ = ColmapKitRunModelConversion
let version = String(cString: ColmapKitVersion())
precondition(!version.isEmpty)
SWIFT

    local simulator_sdk
    simulator_sdk="$(xcrun --sdk iphonesimulator --show-sdk-path)"
    set +e
    xcrun --sdk iphonesimulator swiftc \
      -module-cache-path "$module_cache" \
      -Xcc "-fmodules-cache-path=$module_cache" \
      -sdk "$simulator_sdk" \
      -target "arm64-apple-ios${IOS_DEPLOYMENT_TARGET}-simulator" \
      -F "$XCFRAMEWORK_PATH/ios-arm64-simulator" \
      "$smoke_source" \
      -framework ColmapKit \
      -o "$smoke_binary" > "$smoke_log" 2>&1
    local smoke_status=$?
    set -e
    append_summary "- Swift import/link smoke status: $smoke_status"
    if [[ "$smoke_status" -ne 0 || ! -f "$smoke_binary" ]]; then
      append_summary ""
      append_summary "Last smoke log lines:"
      append_summary ""
      append_summary '```text'
      tail -40 "$smoke_log" >> "$SUMMARY_PATH"
      append_summary '```'
      append_summary ""
      return 1
    fi
    local smoke_build_output
    local smoke_linkage_output
    smoke_build_output="$(xcrun vtool -show-build "$smoke_binary")"
    smoke_linkage_output="$(otool -L "$smoke_binary")"
    printf '%s\n' "$smoke_build_output" > "$smoke_root/vtool.txt"
    printf '%s\n' "$smoke_linkage_output" > "$smoke_root/otool-L.txt"
    if [[ "$smoke_build_output" != *"platform IOSSIMULATOR"* ||
          "$smoke_build_output" != *"minos $IOS_DEPLOYMENT_TARGET"* ]] ||
       ! grep -Fq '@rpath/ColmapKit.framework/ColmapKit' <<< "$smoke_linkage_output"; then
      append_summary "- Swift import/link smoke audit: failed (unexpected platform or framework dependency)"
      return 1
    fi
    append_summary "- Swift import/link smoke audit: passed"
  else
    append_summary "- Swift import/link smoke status: skipped"
  fi

  append_summary "- Result: \`$XCFRAMEWORK_PATH\` is ready for app integration testing"
  append_summary ""
  return 0
}

failures=0

if ! run_slice ios-arm64 iphoneos iOS arm64 "$COLMAPKIT_IOS_DEVICE_VCPKG_TARGET_TRIPLET"; then
  failures=$((failures + 1))
fi

if ! run_slice ios-simulator-arm64 iphonesimulator iOS arm64 "$COLMAPKIT_IOS_SIMULATOR_VCPKG_TARGET_TRIPLET"; then
  failures=$((failures + 1))
fi

if [[ "$failures" -eq 0 &&
      "$COLMAPKIT_IOS_BUILD" == "ON" &&
      "$COLMAPKIT_IOS_PACKAGE" == "ON" ]]; then
  if ! package_xcframework; then
    failures=$((failures + 1))
  fi
fi

append_summary "## Result"
append_summary ""
if [[ "$failures" -eq 0 ]]; then
  if [[ "$COLMAPKIT_IOS_BUILD" == "ON" && "$COLMAPKIT_IOS_PACKAGE" == "ON" ]]; then
    if [[ "$COLMAPKIT_IOS_SMOKE_TEST" == "ON" ]]; then
      append_summary "Both iOS slices built, packaged, audited, and passed the import/link smoke test."
    else
      append_summary "Both iOS slices built, packaged, and audited; the import/link smoke test was skipped."
    fi
  elif [[ "$COLMAPKIT_IOS_BUILD" == "ON" ]]; then
    append_summary "All requested iOS slices configured and built."
  else
    append_summary "All requested iOS slices configured."
  fi
else
  append_summary "$failures requested build or packaging stage(s) failed. Inspect the logs above for concrete blockers."
fi

echo "Wrote $SUMMARY_PATH"

if [[ "$failures" -ne 0 && "$COLMAPKIT_IOS_FAIL_ON_BLOCKER" == "ON" ]]; then
  exit 1
fi
