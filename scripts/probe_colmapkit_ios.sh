#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${COLMAPKIT_IOS_BUILD_ROOT:-"$ROOT_DIR/build-colmapkit-ios-probe"}"
DIST_ROOT="${COLMAPKIT_IOS_DIST_ROOT:-"$ROOT_DIR/dist/colmapkit-ios-probe"}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-18.0}"
COLMAPKIT_CLEAN="${COLMAPKIT_CLEAN:-ON}"
COLMAPKIT_IOS_STRICT_DEPS="${COLMAPKIT_IOS_STRICT_DEPS:-ON}"
COLMAPKIT_IOS_BUILD="${COLMAPKIT_IOS_BUILD:-ON}"
COLMAPKIT_IOS_FAIL_ON_BLOCKER="${COLMAPKIT_IOS_FAIL_ON_BLOCKER:-OFF}"
COLMAPKIT_IOS_USE_VCPKG="${COLMAPKIT_IOS_USE_VCPKG:-OFF}"
COLMAPKIT_VCPKG_ROOT="${COLMAPKIT_VCPKG_ROOT:-${VCPKG_ROOT:-}}"
COLMAPKIT_CMAKE_TOOLCHAIN_FILE="${COLMAPKIT_CMAKE_TOOLCHAIN_FILE:-${CMAKE_TOOLCHAIN_FILE:-}}"
COLMAPKIT_CMAKE_MAKE_PROGRAM="${COLMAPKIT_CMAKE_MAKE_PROGRAM:-${CMAKE_MAKE_PROGRAM:-}}"
COLMAPKIT_VCPKG_INSTALLED_DIR="${COLMAPKIT_VCPKG_INSTALLED_DIR:-${VCPKG_INSTALLED_DIR:-}}"
COLMAPKIT_VCPKG_OVERLAY_TRIPLETS="${COLMAPKIT_VCPKG_OVERLAY_TRIPLETS:-${VCPKG_OVERLAY_TRIPLETS:-}}"
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
- vcpkg mode: $COLMAPKIT_IOS_USE_VCPKG
- vcpkg root: ${COLMAPKIT_VCPKG_ROOT:-unset}
- vcpkg toolchain: ${COLMAPKIT_CMAKE_TOOLCHAIN_FILE:-unset}
- CMake make program: ${COLMAPKIT_CMAKE_MAKE_PROGRAM:-default}
- vcpkg installed dir: ${COLMAPKIT_VCPKG_INSTALLED_DIR:-default}
- vcpkg overlay triplets: ${COLMAPKIT_VCPKG_OVERLAY_TRIPLETS:-unset}
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

failures=0

if ! run_slice ios-arm64 iphoneos iOS arm64 "$COLMAPKIT_IOS_DEVICE_VCPKG_TARGET_TRIPLET"; then
  failures=$((failures + 1))
fi

if ! run_slice ios-simulator-arm64 iphonesimulator iOS arm64 "$COLMAPKIT_IOS_SIMULATOR_VCPKG_TARGET_TRIPLET"; then
  failures=$((failures + 1))
fi

append_summary "## Result"
append_summary ""
if [[ "$failures" -eq 0 ]]; then
  append_summary "All requested iOS slices configured and built."
else
  append_summary "$failures requested iOS slice(s) failed. Inspect the logs above for concrete blockers."
fi

echo "Wrote $SUMMARY_PATH"

if [[ "$failures" -ne 0 && "$COLMAPKIT_IOS_FAIL_ON_BLOCKER" == "ON" ]]; then
  exit 1
fi
