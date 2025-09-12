#!/usr/bin/env bash
# Real-environment test runner for incremental_bitmap plugin
# - Builds with USE_MOCK=OFF and E2E_TDENGINE_REAL_TESTS=ON
# - Expects TDengine 3.x running locally and accessible via taos client

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="$ROOT_DIR/build_real"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TAOSX_PLUGIN=ON \
  -DUSE_MOCK=OFF \
  -DE2E_TDENGINE_REAL_TESTS=ON \
  -DENABLE_TESTS=ON

make -j"$(nproc)"

echo "--- Running real-environment tests ---"
failed=0
for t in test_e2e_tdengine_real test_offset_semantics_real test_offset_semantics_realtime test_tmq_integration; do
  if [[ -x "$t" ]]; then
    echo "== $t =="
    if ! timeout 120s "./$t"; then
      echo "FAILED: $t"
      failed=1
    fi
  else
    echo "MISSING: $t (not built)"
  fi
done

exit "$failed"


