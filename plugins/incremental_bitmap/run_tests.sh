#!/usr/bin/env bash
# One-click mock test runner for incremental_bitmap plugin
# - Builds with USE_MOCK=ON and ENABLE_TESTS=ON
# - Runs all test_* binaries, skipping real-environment tests

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="$ROOT_DIR/build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TAOSX_PLUGIN=ON \
  -DUSE_MOCK=ON \
  -DE2E_TDENGINE_REAL_TESTS=OFF \
  -DENABLE_TESTS=ON

make -j"$(nproc)"

echo "--- Running mock tests (per-test timeout 90s) ---"
failed=0
shopt -s nullglob
for t in test_*; do
  if [[ -x "$t" ]]; then
    # Skip real-environment tests in mock mode
    if [[ "$t" =~ (e2e_tdengine_real|offset_semantics_real|offset_semantics_realtime) ]]; then
      echo "Skipping real-environment test: $t"
      continue
    fi
    echo "== $t =="
    if ! timeout 90s "./$t"; then
      echo "FAILED: $t"
      failed=1
    fi
  fi
done

exit "$failed"


