#!/bin/sh
# Builds and tests the core on the host platform.
set -eu

build_dir="${1:-build-ci}"

cmake -S . -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPIME_BUILD_TESTS=ON \
    -DPIME_BUILD_TOOLS=ON
cmake --build "${build_dir}" -j "${JOBS:-8}"
ctest --test-dir "${build_dir}" --output-on-failure
"${build_dir}/tools/pimeabi" check schema/abi.txt
