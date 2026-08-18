#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${COLMAPKIT_BUILD_ROOT:-"$ROOT_DIR/build-colmapkit-package"}"
DIST_ROOT="${COLMAPKIT_DIST_ROOT:-"$ROOT_DIR/dist/colmapkit"}"
MACOS_BUILD_DIR="$BUILD_ROOT/macos-arm64"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-15.0}"
METAL_ENABLED="${METAL_ENABLED:-ON}"
SIFT_METAL_ENABLED="${SIFT_METAL_ENABLED:-OFF}"
COLMAPKIT_SOURCE_REVISION="${COLMAPKIT_SOURCE_REVISION:-}"
OPENMP_ENABLED="${OPENMP_ENABLED:-ON}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"
CMAKE_C_COMPILER="${CMAKE_C_COMPILER:-/usr/bin/cc}"
CMAKE_CXX_COMPILER="${CMAKE_CXX_COMPILER:-/usr/bin/c++}"
COLMAPKIT_CLEAN="${COLMAPKIT_CLEAN:-ON}"
COLMAPKIT_ALLOW_DEPLOYMENT_MISMATCH="${COLMAPKIT_ALLOW_DEPLOYMENT_MISMATCH:-OFF}"
COLMAPKIT_CMAKE_TOOLCHAIN_FILE="${COLMAPKIT_CMAKE_TOOLCHAIN_FILE:-${CMAKE_TOOLCHAIN_FILE:-}}"
COLMAPKIT_CMAKE_MAKE_PROGRAM="${COLMAPKIT_CMAKE_MAKE_PROGRAM:-${CMAKE_MAKE_PROGRAM:-}}"
COLMAPKIT_VCPKG_TARGET_TRIPLET="${COLMAPKIT_VCPKG_TARGET_TRIPLET:-${VCPKG_TARGET_TRIPLET:-}}"
COLMAPKIT_VCPKG_INSTALLED_DIR="${COLMAPKIT_VCPKG_INSTALLED_DIR:-${VCPKG_INSTALLED_DIR:-}}"
COLMAPKIT_VCPKG_OVERLAY_TRIPLETS="${COLMAPKIT_VCPKG_OVERLAY_TRIPLETS:-${VCPKG_OVERLAY_TRIPLETS:-}}"
COLMAPKIT_IGNORE_PREFIXES="${COLMAPKIT_IGNORE_PREFIXES:-}"
LIBOMP_ROOT="${LIBOMP_ROOT:-}"

if [[ -n "$COLMAPKIT_VCPKG_TARGET_TRIPLET" && -z "$COLMAPKIT_VCPKG_OVERLAY_TRIPLETS" && -d "$ROOT_DIR/cmake/vcpkg-triplets" ]]; then
  COLMAPKIT_VCPKG_OVERLAY_TRIPLETS="$ROOT_DIR/cmake/vcpkg-triplets"
fi

if [[ -z "$LIBOMP_ROOT" ]] && command -v brew >/dev/null 2>&1; then
  LIBOMP_ROOT="$(brew --prefix libomp 2>/dev/null || true)"
fi
if [[ -z "$LIBOMP_ROOT" && -d /opt/homebrew/opt/libomp ]]; then
  LIBOMP_ROOT="/opt/homebrew/opt/libomp"
fi

prepend_cmake_path() {
  local entry="$1"
  local current="$2"
  if [[ -z "$entry" ]]; then
    printf '%s' "$current"
  elif [[ -n "$current" ]]; then
    printf '%s;%s' "$entry" "$current"
  else
    printf '%s' "$entry"
  fi
}

CMAKE_PREFIX_PATH_VALUE="${CMAKE_PREFIX_PATH:-}"
CMAKE_IGNORE_PREFIX_PATH_VALUE="${CMAKE_IGNORE_PREFIX_PATH:-}"
if [[ -n "$LIBOMP_ROOT" ]]; then
  CMAKE_PREFIX_PATH_VALUE="$(prepend_cmake_path "$LIBOMP_ROOT" "$CMAKE_PREFIX_PATH_VALUE")"
fi
if [[ -n "$COLMAPKIT_IGNORE_PREFIXES" ]]; then
  CMAKE_IGNORE_PREFIX_PATH_VALUE="$(prepend_cmake_path "$COLMAPKIT_IGNORE_PREFIXES" "$CMAKE_IGNORE_PREFIX_PATH_VALUE")"
fi
if [[ -d /opt/anaconda3 ]]; then
  CMAKE_IGNORE_PREFIX_PATH_VALUE="$(prepend_cmake_path "/opt/anaconda3" "$CMAKE_IGNORE_PREFIX_PATH_VALUE")"
fi

if [[ "$COLMAPKIT_CLEAN" == "ON" ]]; then
  cmake -E rm -rf "$MACOS_BUILD_DIR"
fi

CMAKE_CONFIGURE_ARGS=(
  cmake -S "$ROOT_DIR" -B "$MACOS_BUILD_DIR" -G "$GENERATOR"
  -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER"
  -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER"
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
  -DCOLMAPKIT_FRAMEWORK_ENABLED=ON \
  -DGUI_ENABLED=OFF \
  -DCUDA_ENABLED=OFF \
  -DOPENGL_ENABLED=OFF \
  -DMVS_ENABLED=OFF \
  -DONNX_ENABLED=OFF \
  -DCGAL_ENABLED=OFF \
  -DDOWNLOAD_ENABLED=OFF \
  -DTESTS_ENABLED=OFF \
  -DOPENMP_ENABLED="$OPENMP_ENABLED" \
  -DMETAL_ENABLED="$METAL_ENABLED" \
  -DSIFT_METAL_ENABLED="$SIFT_METAL_ENABLED" \
  -DCOLMAPKIT_SOURCE_REVISION="$COLMAPKIT_SOURCE_REVISION" \
  -DBUILD_SHARED_LIBS=OFF
)

if [[ -n "$CMAKE_PREFIX_PATH_VALUE" ]]; then
  CMAKE_CONFIGURE_ARGS+=("-DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH_VALUE")
fi
if [[ -n "$CMAKE_IGNORE_PREFIX_PATH_VALUE" ]]; then
  CMAKE_CONFIGURE_ARGS+=("-DCMAKE_IGNORE_PREFIX_PATH=$CMAKE_IGNORE_PREFIX_PATH_VALUE")
fi
if [[ -n "$COLMAPKIT_CMAKE_TOOLCHAIN_FILE" ]]; then
  CMAKE_CONFIGURE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$COLMAPKIT_CMAKE_TOOLCHAIN_FILE")
fi
if [[ -n "$COLMAPKIT_CMAKE_MAKE_PROGRAM" ]]; then
  CMAKE_CONFIGURE_ARGS+=("-DCMAKE_MAKE_PROGRAM=$COLMAPKIT_CMAKE_MAKE_PROGRAM")
fi
if [[ -n "$COLMAPKIT_VCPKG_TARGET_TRIPLET" ]]; then
  CMAKE_CONFIGURE_ARGS+=("-DVCPKG_TARGET_TRIPLET=$COLMAPKIT_VCPKG_TARGET_TRIPLET")
fi
if [[ -n "$COLMAPKIT_VCPKG_INSTALLED_DIR" ]]; then
  CMAKE_CONFIGURE_ARGS+=("-DVCPKG_INSTALLED_DIR=$COLMAPKIT_VCPKG_INSTALLED_DIR")
fi
if [[ -n "$COLMAPKIT_VCPKG_OVERLAY_TRIPLETS" ]]; then
  CMAKE_CONFIGURE_ARGS+=("-DVCPKG_OVERLAY_TRIPLETS=$COLMAPKIT_VCPKG_OVERLAY_TRIPLETS")
fi

if [[ -n "$LIBOMP_ROOT" ]]; then
  CMAKE_CONFIGURE_ARGS+=("-DOpenMP_ROOT=$LIBOMP_ROOT")
  CMAKE_CONFIGURE_ARGS+=("-DOpenMP_C_FLAGS=-Xpreprocessor -fopenmp -I$LIBOMP_ROOT/include")
  CMAKE_CONFIGURE_ARGS+=("-DOpenMP_CXX_FLAGS=-Xpreprocessor -fopenmp -I$LIBOMP_ROOT/include")
  CMAKE_CONFIGURE_ARGS+=("-DOpenMP_C_LIB_NAMES=omp")
  CMAKE_CONFIGURE_ARGS+=("-DOpenMP_CXX_LIB_NAMES=omp")
  CMAKE_CONFIGURE_ARGS+=("-DOpenMP_omp_LIBRARY=$LIBOMP_ROOT/lib/libomp.dylib")
fi

"${CMAKE_CONFIGURE_ARGS[@]}"

cmake --build "$MACOS_BUILD_DIR" --target ColmapKit

FRAMEWORK_PATH="$MACOS_BUILD_DIR/src/colmap/colmapkit/ColmapKit.framework"
FRAMEWORK_BINARY="$FRAMEWORK_PATH/Versions/A/ColmapKit"
FRAMEWORK_DEPENDENCIES_DIR="$FRAMEWORK_PATH/Versions/A/Frameworks"
FRAMEWORK_MODULES_LINK="$FRAMEWORK_PATH/Modules"
FRAMEWORK_MODULES_DIR="$FRAMEWORK_PATH/Versions/A/Modules"
XCFRAMEWORK_PATH="$DIST_ROOT/ColmapKit.xcframework"
AUDIT_PATH="$DIST_ROOT/ColmapKit-otool-L.txt"
SIGNATURE_AUDIT_PATH="$DIST_ROOT/ColmapKit-codesign.txt"
DEPLOYMENT_AUDIT_PATH="$DIST_ROOT/ColmapKit-deployment-targets.txt"
DEPLOYMENT_MISMATCH_PATH="$DIST_ROOT/ColmapKit-deployment-mismatches.txt"

if [[ ! -f "$FRAMEWORK_BINARY" ]]; then
  echo "error: Could not find ColmapKit framework binary at $FRAMEWORK_BINARY" >&2
  exit 1
fi

is_vendored_dependency() {
  local dependency="$1"
  [[ "$dependency" == /* ]] || return 1
  [[ "$dependency" != /System/* ]] || return 1
  [[ "$dependency" != /usr/lib/* ]] || return 1
  [[ -f "$dependency" ]] || return 1
}

contains_dependency() {
  local needle="$1"
  local dependency
  local found=1
  set +u
  for dependency in "${VENDORED_DEPENDENCIES[@]}"; do
    if [[ "$dependency" == "$needle" ]]; then
      found=0
      break
    fi
  done
  set -u
  return "$found"
}

add_vendored_dependency() {
  local dependency="$1"
  if ! contains_dependency "$dependency"; then
    VENDORED_DEPENDENCIES+=("$dependency")
    DEPENDENCY_QUEUE+=("$dependency")
  fi
}

collect_vendored_dependencies_from() {
  local binary="$1"
  local dependency
  while IFS= read -r dependency; do
    if is_vendored_dependency "$dependency"; then
      add_vendored_dependency "$dependency"
    fi
  done < <(otool -L "$binary" | awk 'NR > 1 { print $1 }')
}

rewrite_dependency_in_binary() {
  local binary="$1"
  local old_dependency="$2"
  local new_dependency="$3"
  if otool -L "$binary" | awk 'NR > 1 { print $1 }' | grep -Fxq "$old_dependency"; then
    install_name_tool -change "$old_dependency" "$new_dependency" "$binary"
  fi
}

VENDORED_DEPENDENCIES=()
DEPENDENCY_QUEUE=("$FRAMEWORK_BINARY")
if [[ -n "$LIBOMP_ROOT" && -f "$LIBOMP_ROOT/lib/libomp.dylib" ]] && \
  otool -L "$FRAMEWORK_BINARY" | awk 'NR > 1 { print $1 }' | grep -Fxq "@rpath/libomp.dylib"; then
  add_vendored_dependency "$LIBOMP_ROOT/lib/libomp.dylib"
fi
queue_index=0
while [[ "$queue_index" -lt "${#DEPENDENCY_QUEUE[@]}" ]]; do
  collect_vendored_dependencies_from "${DEPENDENCY_QUEUE[$queue_index]}"
  queue_index=$((queue_index + 1))
done

if [[ "${#VENDORED_DEPENDENCIES[@]}" -gt 0 ]]; then
  rm -rf "$FRAMEWORK_DEPENDENCIES_DIR"
  mkdir -p "$FRAMEWORK_DEPENDENCIES_DIR"

  for dependency in "${VENDORED_DEPENDENCIES[@]}"; do
    copied_dependency="$FRAMEWORK_DEPENDENCIES_DIR/$(basename "$dependency")"
    if [[ ! -e "$copied_dependency" ]]; then
      cp -p "$dependency" "$copied_dependency"
      chmod u+w "$copied_dependency"
    fi
  done

  for dependency in "${VENDORED_DEPENDENCIES[@]}"; do
    dependency_install_name="$(otool -D "$dependency" | awk 'NR == 2 { print $1 }')"
    rewrite_dependency_in_binary \
      "$FRAMEWORK_BINARY" \
      "$dependency" \
      "@loader_path/Frameworks/$(basename "$dependency")"
    if [[ -n "$dependency_install_name" && "$dependency_install_name" != "$dependency" ]]; then
      rewrite_dependency_in_binary \
        "$FRAMEWORK_BINARY" \
        "$dependency_install_name" \
        "@loader_path/Frameworks/$(basename "$dependency")"
    fi
  done

  for dependency in "${VENDORED_DEPENDENCIES[@]}"; do
    copied_dependency="$FRAMEWORK_DEPENDENCIES_DIR/$(basename "$dependency")"
    install_name_tool -id "@loader_path/$(basename "$dependency")" "$copied_dependency"
    for nested_dependency in "${VENDORED_DEPENDENCIES[@]}"; do
      nested_dependency_install_name="$(otool -D "$nested_dependency" | awk 'NR == 2 { print $1 }')"
      rewrite_dependency_in_binary \
        "$copied_dependency" \
        "$nested_dependency" \
        "@loader_path/$(basename "$nested_dependency")"
      if [[ -n "$nested_dependency_install_name" && "$nested_dependency_install_name" != "$nested_dependency" ]]; then
        rewrite_dependency_in_binary \
          "$copied_dependency" \
          "$nested_dependency_install_name" \
          "@loader_path/$(basename "$nested_dependency")"
      fi
    done
    codesign --force --sign - "$copied_dependency"
  done
fi

if [[ -d "$FRAMEWORK_MODULES_LINK" && ! -L "$FRAMEWORK_MODULES_LINK" ]]; then
  rm -rf "$FRAMEWORK_MODULES_DIR"
  mv "$FRAMEWORK_MODULES_LINK" "$FRAMEWORK_MODULES_DIR"
  ln -s Versions/Current/Modules "$FRAMEWORK_MODULES_LINK"
fi

codesign --force --deep --sign - "$FRAMEWORK_PATH"

rm -rf "$XCFRAMEWORK_PATH"
mkdir -p "$DIST_ROOT"

xcodebuild -create-xcframework \
  -framework "$FRAMEWORK_PATH" \
  -output "$XCFRAMEWORK_PATH"

XCFRAMEWORK_BINARY="$(find "$XCFRAMEWORK_PATH" -type f -path '*/ColmapKit.framework/Versions/A/ColmapKit' -print -quit)"
if [[ -z "$XCFRAMEWORK_BINARY" ]]; then
  XCFRAMEWORK_BINARY="$(find "$XCFRAMEWORK_PATH" -type f -path '*/ColmapKit.framework/ColmapKit' -print -quit)"
fi
if [[ -z "$XCFRAMEWORK_BINARY" ]]; then
  echo "error: Could not find ColmapKit framework binary in $XCFRAMEWORK_PATH" >&2
  exit 1
fi

find "$XCFRAMEWORK_PATH" -type f -name '*.dylib' -exec codesign --force --sign - {} \;
find "$XCFRAMEWORK_PATH" -type d -name 'ColmapKit.framework' -exec codesign --force --deep --sign - {} \;
codesign --force --sign - "$XCFRAMEWORK_PATH"

otool -L "$XCFRAMEWORK_BINARY" > "$AUDIT_PATH"
codesign --verify --deep --strict --verbose=2 "$XCFRAMEWORK_PATH" > "$SIGNATURE_AUDIT_PATH" 2>&1
{
  echo "ColmapKit:"
  vtool -show-build "$XCFRAMEWORK_BINARY"
  echo
  echo "Vendored dylibs:"
  find "$XCFRAMEWORK_PATH" -type f -name '*.dylib' -print0 | while IFS= read -r -d '' dylib; do
    minos="$(vtool -show-build "$dylib" 2>/dev/null | awk '/minos/ { print $2; exit }')"
    printf '%s %s\n' "${minos:-unknown}" "$dylib"
  done | sort
} > "$DEPLOYMENT_AUDIT_PATH"

awk -v target="$MACOS_DEPLOYMENT_TARGET" '
function split_version(version, parts) {
  count = split(version, parts, ".")
  for (part_index = count + 1; part_index <= 3; part_index++) {
    parts[part_index] = 0
  }
}
function version_greater_than(lhs, rhs, lhs_parts, rhs_parts, part_index) {
  split_version(lhs, lhs_parts)
  split_version(rhs, rhs_parts)
  for (part_index = 1; part_index <= 3; part_index++) {
    if (lhs_parts[part_index] + 0 > rhs_parts[part_index] + 0) {
      return 1
    }
    if (lhs_parts[part_index] + 0 < rhs_parts[part_index] + 0) {
      return 0
    }
  }
  return 0
}
$1 ~ /^[0-9]+(\.[0-9]+)*$/ && $2 ~ /\.dylib$/ && version_greater_than($1, target) {
  print
}
' "$DEPLOYMENT_AUDIT_PATH" > "$DEPLOYMENT_MISMATCH_PATH"
deployment_mismatch_count="$(wc -l < "$DEPLOYMENT_MISMATCH_PATH" | tr -d ' ')"

cat > "$DIST_ROOT/README.md" <<README
# ColmapKit.xcframework

Built from:

\`\`\`text
$ROOT_DIR
\`\`\`

Primary artifact:

\`\`\`text
$XCFRAMEWORK_PATH
\`\`\`

Build inputs:

\`\`\`text
MACOS_DEPLOYMENT_TARGET=$MACOS_DEPLOYMENT_TARGET
LIBOMP_ROOT=${LIBOMP_ROOT:-<none>}
COLMAPKIT_CMAKE_TOOLCHAIN_FILE=${COLMAPKIT_CMAKE_TOOLCHAIN_FILE:-<none>}
COLMAPKIT_VCPKG_TARGET_TRIPLET=${COLMAPKIT_VCPKG_TARGET_TRIPLET:-<none>}
COLMAPKIT_VCPKG_INSTALLED_DIR=${COLMAPKIT_VCPKG_INSTALLED_DIR:-<none>}
COLMAPKIT_VCPKG_OVERLAY_TRIPLETS=${COLMAPKIT_VCPKG_OVERLAY_TRIPLETS:-<none>}
\`\`\`

Dynamic dependency audit:

\`\`\`text
$AUDIT_PATH
\`\`\`

Signature audit:

\`\`\`text
$SIGNATURE_AUDIT_PATH
\`\`\`

Deployment target audit:

\`\`\`text
$DEPLOYMENT_AUDIT_PATH
\`\`\`

Deployment mismatches:

\`\`\`text
$DEPLOYMENT_MISMATCH_PATH
\`\`\`

Review \`ColmapKit-otool-L.txt\` before shipping. Non-system dylibs are vendored
inside \`ColmapKit.framework/Versions/A/Frameworks\` and rewritten to
\`@loader_path\`. Review \`ColmapKit-deployment-targets.txt\` before shipping;
vendored Homebrew dylibs may require a newer macOS version than the framework
deployment target.
README

echo "Created $XCFRAMEWORK_PATH"
echo "Wrote dependency audit to $AUDIT_PATH"
echo "Wrote signature audit to $SIGNATURE_AUDIT_PATH"
echo "Wrote deployment target audit to $DEPLOYMENT_AUDIT_PATH"
echo "Wrote deployment mismatch audit to $DEPLOYMENT_MISMATCH_PATH"

if [[ "$deployment_mismatch_count" != "0" ]]; then
  if [[ "$COLMAPKIT_ALLOW_DEPLOYMENT_MISMATCH" == "ON" ]]; then
    echo "warning: $deployment_mismatch_count vendored dylibs require a newer macOS version than $MACOS_DEPLOYMENT_TARGET" >&2
  else
    echo "error: $deployment_mismatch_count vendored dylibs require a newer macOS version than $MACOS_DEPLOYMENT_TARGET" >&2
    echo "Set COLMAPKIT_ALLOW_DEPLOYMENT_MISMATCH=ON only for local proof artifacts that will not be vendored into Splats." >&2
    exit 1
  fi
fi
