#!/bin/sh
# Build only the headless server and its network checks from local source.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=${1:-"$repo/work/server-linux"}
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DGT2_BUILD_XRSIM=OFF
cmake --build "$build" --parallel 2 --target gt2lobby gt2lobbychecks gt2server gt2netcheck gt2networkchecks gt2privatechecks gt2hydrogenchecks gt2transportcheck
ctest --test-dir "$build" --output-on-failure -R '^(lobby_checks|network_checks|network_transport|private_transport_checks|hydrogen_upstream_checks)$'
printf '\nBuilt server: %s/gt2server\n' "$build"
