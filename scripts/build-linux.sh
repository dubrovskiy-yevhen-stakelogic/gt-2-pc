#!/bin/bash
set -euo pipefail
gt2_root="$(cd "$(dirname "$0")/.." && pwd)"
if [ "$(uname -s)" != Linux ]; then
    echo 'This script requires desktop Linux.' >&2
    exit 1
fi
gt2_build="${GT2_BUILD_DIR:-$gt2_root/build_linux}"
cmake -S "$gt2_root" -B "$gt2_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DGT2_BUILD_LINUX_CLIENT=ON -DBUILD_TESTING=ON "$@"
cmake --build "$gt2_build" --parallel "${GT2_BUILD_JOBS:-4}"
ctest --test-dir "$gt2_build" --output-on-failure
printf '\nBuild and checks passed. Launch: "%s/gt2launcher"\n' "$gt2_build"
