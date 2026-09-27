#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")" || exit 1
gt2_play() {
    if [ "$(uname -s)" != Darwin ]; then
        echo 'This launcher requires macOS.' >&2
        return 1
    fi
    # Finder starts a fresh shell without the installer's dependency variables.
    # The installed bundle has its own loader/driver and native disc importer.
    if [ -z "${GT2_BUILD_DIR:-}" ]; then
        for gt2_app in "$HOME/Applications/GT2.app" /Applications/GT2.app; do
            if [ -x "$gt2_app/Contents/MacOS/GT2" ]; then
                "$gt2_app/Contents/MacOS/GT2" "$@"
                return $?
            fi
        done
    fi
    if [ -f scripts/play-macos.sh ]; then
        bash scripts/play-macos.sh "$@"
    else
        echo 'GT2.app was not found. Run INSTALL-MACOS.command from the complete installer archive and finish its application packaging step.' >&2
        return 1
    fi
}
gt2_play "$@"
gt2_result=$?
if [ "$gt2_result" -ne 0 ]; then
    printf '\nGame exited with an error.\n'
    if [ -t 0 ]; then printf 'Press Enter to close.\n'; read -r _; fi
fi
exit "$gt2_result"
