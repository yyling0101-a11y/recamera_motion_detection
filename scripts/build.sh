#!/usr/bin/env bash
# Cross-compile motion_detection for reCamera (SG2002, riscv64 musl).
#
# Required layout (see README):
#   SSCMA_EXAMPLE_PATH  sscma-example-sg200x checkout (provides cmake/ + components/)
#   SG200X_SDK_PATH     reCamera-OS SDK output, e.g. .../sg2002_recamera_emmc
#   HOST_TOOLS_PATH     directory containing gcc/riscv64-linux-musl-x86_64/bin
#
# The project is copied into $SSCMA_EXAMPLE_PATH/solutions/motion_detection
# because the CMake glue resolves the SDK build system through ../../.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SSCMA_EXAMPLE_PATH="${SSCMA_EXAMPLE_PATH:-$HOME/work/sscma-example-sg200x}"
HOST_TOOLS_PATH="${HOST_TOOLS_PATH:-$HOME/work/host-tools}"
SG200X_SDK_PATH="${SG200X_SDK_PATH:-$HOME/work/sdk_dl/sg2002_recamera_emmc}"
BUILD_DIR="${BUILD_DIR:-$SSCMA_EXAMPLE_PATH/solutions/motion_detection}"

for required in "$SSCMA_EXAMPLE_PATH/cmake/project.cmake" "$SG200X_SDK_PATH/cvi_mpi/include" \
                "$HOST_TOOLS_PATH/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-g++"; do
  if [[ ! -e "$required" ]]; then
    echo "missing dependency: $required" >&2
    echo "set SSCMA_EXAMPLE_PATH / SG200X_SDK_PATH / HOST_TOOLS_PATH" >&2
    exit 1
  fi
done

mkdir -p "$BUILD_DIR"
cp -a "$REPO_DIR/CMakeLists.txt" "$BUILD_DIR/"
mkdir -p "$BUILD_DIR/main"
cp -a "$REPO_DIR/main/." "$BUILD_DIR/main/"

export SG200X_SDK_PATH
export PATH="$HOST_TOOLS_PATH/gcc/riscv64-linux-musl-x86_64/bin:$PATH"

cmake -S "$BUILD_DIR" -B "$BUILD_DIR/build" -DCMAKE_BUILD_TYPE=Release >"$BUILD_DIR/build.configure.log" 2>&1
cmake --build "$BUILD_DIR/build" -j"$(nproc)"

BIN="$BUILD_DIR/build/motion_detection"
echo "built: $BIN"
file "$BIN" 2>/dev/null || true
