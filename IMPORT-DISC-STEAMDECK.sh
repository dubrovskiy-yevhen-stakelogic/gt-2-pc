#!/bin/bash
set -eu
gt2_root="$(cd "$(dirname "$0")" && pwd)"
exec /bin/bash "$gt2_root/PLAY-STEAMDECK.sh" --import
