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
XCFRAMEWORK_PATH="$DIST_ROOT/ColmapKit.xcframework"
AUDIT_PATH="$DIST_ROOT/ColmapKit-otool-L.txt"

rm -rf "$XCFRAMEWORK_PATH"
mkdir -p "$DIST_ROOT"

xcodebuild -create-xcframework \
  -framework "$FRAMEWORK_PATH" \
  -output "$XCFRAMEWORK_PATH"

FRAMEWORK_BINARY="$(find "$XCFRAMEWORK_PATH" -type f -path '*/ColmapKit.framework/Versions/A/ColmapKit' -print -quit)"
if [[ -z "$FRAMEWORK_BINARY" ]]; then
  FRAMEWORK_BINARY="$(find "$XCFRAMEWORK_PATH" -type f -path '*/ColmapKit.framework/ColmapKit' -print -quit)"
fi
if [[ -z "$FRAMEWORK_BINARY" ]]; then
  echo "error: Could not find ColmapKit framework binary in $XCFRAMEWORK_PATH" >&2
  exit 1
fi

otool -L "$FRAMEWORK_BINARY" > "$AUDIT_PATH"

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

Review \`ColmapKit-otool-L.txt\` before shipping. A production package should
not accidentally depend on undeclared Homebrew dylibs.
README

echo "Created $XCFRAMEWORK_PATH"
echo "Wrote dependency audit to $AUDIT_PATH"
