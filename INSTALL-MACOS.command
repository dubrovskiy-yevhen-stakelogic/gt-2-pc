#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")" || exit 1
bash scripts/install-macos.sh "$@"
gt2_result=$?
if [ "$gt2_result" -ne 0 ]; then
    printf '\nInstallation stopped. Your saves have not been removed. See the error above.\n'
    printf 'An existing GT2.app may still be the previous version; this update did not finish.\n'
fi
if [ -t 0 ]; then printf '\nPress Enter to close.\n'; read -r _; fi
exit "$gt2_result"
