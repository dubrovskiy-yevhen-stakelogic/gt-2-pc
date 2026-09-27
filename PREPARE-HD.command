#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")" || exit 1
if [ "$(uname -s)" != Darwin ]; then echo 'This command requires macOS.' >&2; exit 1; fi
gt2_python=''
for candidate in /opt/homebrew/opt/python@3.13/bin/python3.13 /usr/local/opt/python@3.13/bin/python3.13 python3; do
    if command -v "$candidate" >/dev/null 2>&1; then gt2_python="$candidate"; break; fi
done
if [ -z "$gt2_python" ]; then
    echo 'Python 3 is required. Run INSTALL-MACOS.command first.' >&2
    exit 1
fi
"$gt2_python" scripts/prepare-hd-macos.py "$@"
gt2_result=$?
if [ "$gt2_result" -ne 0 ]; then echo 'HD preparation stopped. Original discs and saves were retained.'; fi
if [ -t 0 ]; then printf '\nPress Enter to close.\n'; read -r _; fi
exit "$gt2_result"
