#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if ! command -v python3 >/dev/null 2>&1; then
    echo "Python 3.10+ is required. See docs/LINUX-INSTALL.md."
    exit 1
fi
if ! python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))'; then
    echo "Python 3.10 or newer is required. See docs/LINUX-INSTALL.md."
    exit 1
fi
exec python3 scripts/install-linux.py "$@"
