#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-15.0}"
LIBOMP_BUILD_ROOT="${LIBOMP_BUILD_ROOT:-"$ROOT_DIR/build-libomp-macos"}"
LIBOMP_INSTALL_PREFIX="${LIBOMP_INSTALL_PREFIX:-"$ROOT_DIR/dist/libomp-macos$MACOS_DEPLOYMENT_TARGET"}"
LIBOMP_SOURCE_ARCHIVE="${LIBOMP_SOURCE_ARCHIVE:-}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"
COLMAPKIT_CMAKE_MAKE_PROGRAM="${COLMAPKIT_CMAKE_MAKE_PROGRAM:-${CMAKE_MAKE_PROGRAM:-}}"

if [[ -z "$LIBOMP_SOURCE_ARCHIVE" ]]; then
  if ! command -v brew >/dev/null 2>&1; then
    echo "error: LIBOMP_SOURCE_ARCHIVE is required when Homebrew is unavailable" >&2
    exit 1
  fi
  LIBOMP_SOURCE_ARCHIVE="$(brew --cache --build-from-source libomp)"
  if [[ ! -f "$LIBOMP_SOURCE_ARCHIVE" ]]; then
    brew fetch --build-from-source libomp
    LIBOMP_SOURCE_ARCHIVE="$(brew --cache --build-from-source libomp)"
  fi
fi

if [[ ! -f "$LIBOMP_SOURCE_ARCHIVE" ]]; then
  echo "error: Could not find libomp source archive at $LIBOMP_SOURCE_ARCHIVE" >&2
  exit 1
fi

cmake -E rm -rf "$LIBOMP_BUILD_ROOT"
cmake -E make_directory "$LIBOMP_BUILD_ROOT/src"
tar -xf "$LIBOMP_SOURCE_ARCHIVE" -C "$LIBOMP_BUILD_ROOT/src"

LLVM_SOURCE_DIR="$(find "$LIBOMP_BUILD_ROOT/src" -maxdepth 1 -type d -name 'llvm-project*.src' -print -quit)"
if [[ -z "$LLVM_SOURCE_DIR" || ! -d "$LLVM_SOURCE_DIR/runtimes" ]]; then
  echo "error: Could not find llvm-project runtimes source under $LIBOMP_BUILD_ROOT/src" >&2
  exit 1
fi

CMAKE_CONFIGURE_ARGS=(
  cmake -S "$LLVM_SOURCE_DIR/runtimes" -B "$LIBOMP_BUILD_ROOT/shared" -G "$GENERATOR"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_OSX_ARCHITECTURES=arm64
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"
  -DCMAKE_INSTALL_PREFIX="$LIBOMP_INSTALL_PREFIX"
  -DLIBOMP_INSTALL_ALIASES=OFF
  -DLLVM_ENABLE_RUNTIMES=openmp
  -DOPENMP_ENABLE_OMPT_TOOLS=OFF
  -DOPENMP_ENABLE_LIBOMPTARGET=OFF
  -DLIBOMP_ENABLE_SHARED=ON
)

if [[ -n "$COLMAPKIT_CMAKE_MAKE_PROGRAM" ]]; then
  CMAKE_CONFIGURE_ARGS+=("-DCMAKE_MAKE_PROGRAM=$COLMAPKIT_CMAKE_MAKE_PROGRAM")
fi

"${CMAKE_CONFIGURE_ARGS[@]}"
cmake --build "$LIBOMP_BUILD_ROOT/shared" --target install

LIBOMP_DYLIB="$LIBOMP_INSTALL_PREFIX/lib/libomp.dylib"
if [[ ! -f "$LIBOMP_DYLIB" ]]; then
  echo "error: Could not find built libomp dylib at $LIBOMP_DYLIB" >&2
  exit 1
fi

codesign --force --sign - "$LIBOMP_DYLIB"
vtool -show-build "$LIBOMP_DYLIB" > "$LIBOMP_INSTALL_PREFIX/libomp-deployment-target.txt"

echo "Built $LIBOMP_DYLIB"
echo "Wrote deployment audit to $LIBOMP_INSTALL_PREFIX/libomp-deployment-target.txt"
