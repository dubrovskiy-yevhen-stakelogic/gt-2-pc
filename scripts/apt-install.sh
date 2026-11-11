#!/bin/bash
# Hosted Ubuntu runners occasionally lose DNS or the Azure mirror mid-apt.
# Retry the whole update+install with a bounded timeout instead of failing a
# release job on a transient mirror error.
set -euo pipefail
if [ "$#" -eq 0 ]; then
    echo "usage: apt-install.sh package..." >&2
    exit 2
fi
for attempt in 1 2 3 4; do
    if sudo timeout 300 apt-get -o Acquire::Retries=3 update &&
       sudo timeout 300 apt-get -o Acquire::Retries=3 install --yes --no-install-recommends "$@"; then
        exit 0
    fi
    if [ "$attempt" -eq 4 ]; then
        echo "apt install failed after $attempt attempts" >&2
        exit 1
    fi
    echo "apt attempt $attempt failed; retrying in $((attempt * 15)) seconds" >&2
    sleep $((attempt * 15))
done
