#!/usr/bin/env bash
# Builds BiSheng C toolchain (clang + libcbs) from a checked-out source
# tree. Called by .github/workflows/build-compiler-release.yml.
#
# Usage: bash release-build.sh <src_dir> <install_dir>
# Env: BUILD_TYPE (Release), LLVM_TARGETS (X86), CCACHE_DIR (~/.ccache).

set -euo pipefail

SRC_DIR="${1:?usage: release-build.sh <src_dir> <install_dir>}"
INSTALL_DIR="${2:?usage: release-build.sh <src_dir> <install_dir>}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
LLVM_TARGETS="${LLVM_TARGETS:-X86}"

if [ ! -d "$SRC_DIR" ]; then
  echo "release-build: source dir '$SRC_DIR' does not exist" >&2
  exit 1
fi
SRC_DIR="$(cd "$SRC_DIR" && pwd)"

mkdir -p "$INSTALL_DIR"
INSTALL_DIR="$(cd "$INSTALL_DIR" && pwd)"

# Absolute CCACHE_DIR; HOME is overridden by actions/checkout on CI.
: "${CCACHE_DIR:=$HOME/.ccache}"
mkdir -p "$CCACHE_DIR"
export CCACHE_DIR

BUILD_DIR="$SRC_DIR/build-release"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "[release-build] src=$SRC_DIR install=$INSTALL_DIR build_type=$BUILD_TYPE targets=$LLVM_TARGETS"

# gcc-13: LLVM 15.0.4 needs transitive <cstdint> dropped by GCC 14+.
# lld: faster final link. ccache: incremental builds.
cmake -G "Ninja" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_C_COMPILER=gcc-13 \
  -DCMAKE_CXX_COMPILER=g++-13 \
  -DLLVM_TARGETS_TO_BUILD="$LLVM_TARGETS" \
  -DBUILD_SHARED_LIBS=OFF \
  -DLLVM_ENABLE_PROJECTS="clang;libcbs" \
  -DENABLE_BSC_LIB_FUTURE=ON \
  -DLLVM_USE_LINKER=lld \
  -DLLVM_CCACHE_BUILD=ON \
  -DLLVM_BUILD_DOCS=Off \
  -DLLVM_ENABLE_BINDINGS=Off \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DENABLE_BSC=ON \
  "$SRC_DIR/llvm"

NUM_JOBS="$(grep -c ^processor /proc/cpuinfo)"
ninja -j"$NUM_JOBS" clang
ninja -j"$NUM_JOBS" stdcbs

# Wipe first so INSTALL_DIR stays deterministic across reruns.
rm -rf "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
ninja install

# Trim the install tree — drop llvm-* tools, dev archives, cmake config,
# etc. Keep only clang front-end binaries in bin/; clang/ (resource
# headers) and libstdcbs.a in lib/.
clean_install() {
  local temp="$INSTALL_DIR/.tmp-clean"
  mkdir -p "$temp"

  cd "$INSTALL_DIR/bin"
  mv clang-[0-9]* clang clang++ clang-cl clang-cpp clang-check clang-format "$temp" 2>/dev/null || true
  rm -rf "$INSTALL_DIR/bin"/*
  mv "$temp"/* "$INSTALL_DIR/bin"/

  cd "$INSTALL_DIR/lib"
  mv clang libstdcbs.a "$temp" 2>/dev/null || true
  rm -rf "$INSTALL_DIR/lib"/*
  mv "$temp"/* "$INSTALL_DIR/lib"/

  rm -rf "$temp"
}
clean_install

echo "[release-build] done: $INSTALL_DIR"
