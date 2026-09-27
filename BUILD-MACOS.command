#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")" || exit 1
bash scripts/build-macos.sh
gt2_result=$?
if [ "$gt2_result" -ne 0 ]; then
    printf '\nBuild failed. See the error above and docs/MACOS.md. Press Enter to close.\n'
    read -r _
fi
exit "$gt2_result"
