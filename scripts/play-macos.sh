#!/bin/bash
set -euo pipefail
gt2_root="$(cd "$(dirname "$0")/.." && pwd)"
source "$gt2_root/scripts/macos-env.sh"
gt2_build="${GT2_BUILD_DIR:-$gt2_root/build_macos}"
if [ ! -x "$gt2_build/gt2game" ]; then
    echo 'Build the game first: bash BUILD-MACOS.command' >&2
    exit 1
fi
gt2_data="${GT2_DATA_ROOT:-$HOME/Library/Application Support/GT2}"
if [ -z "${GT2_DATA_ROOT:-}" ] && { [ -d "$gt2_root/runtime/arcade" ] || [ -d "$gt2_root/runtime/simulation" ]; }; then
    gt2_data="$gt2_root/runtime"
fi
# Keep Mac saves separate even when reusing a copied Windows runtime folder.
gt2_saves="${GT2_SAVE_ROOT:-$HOME/Library/Application Support/GT2/saves}"
mkdir -p "$gt2_saves"
cd "$gt2_root"
exec "$gt2_build/gt2game" --data-root "$gt2_data" --save-root "$gt2_saves" "$@"
