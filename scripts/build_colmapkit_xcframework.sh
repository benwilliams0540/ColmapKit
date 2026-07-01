#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${COLMAPKIT_BUILD_ROOT:-"$ROOT_DIR/build-colmapkit-package"}"
DIST_ROOT="${COLMAPKIT_DIST_ROOT:-"$ROOT_DIR/dist/colmapkit"}"
MACOS_BUILD_DIR="$BUILD_ROOT/macos-arm64"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-13.0}"
METAL_ENABLED="${METAL_ENABLED:-ON}"
SIFT_METAL_ENABLED="${SIFT_METAL_ENABLED:-OFF}"
OPENMP_ENABLED="${OPENMP_ENABLED:-ON}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"
CMAKE_C_COMPILER="${CMAKE_C_COMPILER:-/usr/bin/cc}"
CMAKE_CXX_COMPILER="${CMAKE_CXX_COMPILER:-/usr/bin/c++}"
COLMAPKIT_CLEAN="${COLMAPKIT_CLEAN:-ON}"
LIBOMP_ROOT="${LIBOMP_ROOT:-}"

if [[ -z "$LIBOMP_ROOT" ]] && command -v brew >/dev/null 2>&1; then
  LIBOMP_ROOT="$(brew --prefix libomp 2>/dev/null || true)"
fi
if [[ -z "$LIBOMP_ROOT" && -d /opt/homebrew/opt/libomp ]]; then
  LIBOMP_ROOT="/opt/homebrew/opt/libomp"
fi

CMAKE_PREFIX_PATH_VALUE="${CMAKE_PREFIX_PATH:-}"
CMAKE_IGNORE_PREFIX_PATH_VALUE="${CMAKE_IGNORE_PREFIX_PATH:-}"
if [[ -n "$LIBOMP_ROOT" ]]; then
  if [[ -n "$CMAKE_PREFIX_PATH_VALUE" ]]; then
    CMAKE_PREFIX_PATH_VALUE="$LIBOMP_ROOT;$CMAKE_PREFIX_PATH_VALUE"
  else
    CMAKE_PREFIX_PATH_VALUE="$LIBOMP_ROOT"
  fi
fi
if [[ -d /opt/anaconda3 ]]; then
  if [[ -n "$CMAKE_IGNORE_PREFIX_PATH_VALUE" ]]; then
    CMAKE_IGNORE_PREFIX_PATH_VALUE="/opt/anaconda3;$CMAKE_IGNORE_PREFIX_PATH_VALUE"
  else
    CMAKE_IGNORE_PREFIX_PATH_VALUE="/opt/anaconda3"
  fi
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
  -DBUILD_SHARED_LIBS=OFF
)

if [[ -n "$CMAKE_PREFIX_PATH_VALUE" ]]; then
  CMAKE_CONFIGURE_ARGS+=("-DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH_VALUE")
fi
if [[ -n "$CMAKE_IGNORE_PREFIX_PATH_VALUE" ]]; then
  CMAKE_CONFIGURE_ARGS+=("-DCMAKE_IGNORE_PREFIX_PATH=$CMAKE_IGNORE_PREFIX_PATH_VALUE")
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

collect_vendored_dependencies_from() {
  local binary="$1"
  local dependency
  while IFS= read -r dependency; do
    if is_vendored_dependency "$dependency" && ! contains_dependency "$dependency"; then
      VENDORED_DEPENDENCIES+=("$dependency")
      DEPENDENCY_QUEUE+=("$dependency")
    fi
  done < <(otool -L "$binary" | awk 'NR > 1 { print $1 }')
}

VENDORED_DEPENDENCIES=()
DEPENDENCY_QUEUE=("$FRAMEWORK_BINARY")
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
    install_name_tool \
      -change "$dependency" "@loader_path/Frameworks/$(basename "$dependency")" \
      "$FRAMEWORK_BINARY"
  done

  for dependency in "${VENDORED_DEPENDENCIES[@]}"; do
    copied_dependency="$FRAMEWORK_DEPENDENCIES_DIR/$(basename "$dependency")"
    install_name_tool -id "@loader_path/$(basename "$dependency")" "$copied_dependency"
    for nested_dependency in "${VENDORED_DEPENDENCIES[@]}"; do
      if otool -L "$copied_dependency" | awk 'NR > 1 { print $1 }' | grep -Fxq "$nested_dependency"; then
        install_name_tool \
          -change "$nested_dependency" "@loader_path/$(basename "$nested_dependency")" \
          "$copied_dependency"
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
