#!/bin/bash
set -Eeuo pipefail
gt2_root="$(cd "$(dirname "$0")" && pwd)"
if [ "${1:-}" = --gt2-terminal ]; then
    shift
elif [ ! -t 0 ] || [ ! -t 1 ]; then
    if command -v konsole >/dev/null; then
        exec konsole --separate --hold -e /bin/bash "$gt2_root/INSTALL-STEAMDECK.sh" --gt2-terminal "$@"
    elif command -v xterm >/dev/null; then
        exec xterm -hold -e /bin/bash "$gt2_root/INSTALL-STEAMDECK.sh" --gt2-terminal "$@"
    else
        gt2_error='GT2 cannot open a terminal window. Run this installer in Steam Deck Desktop Mode.'
        if command -v kdialog >/dev/null; then kdialog --error "$gt2_error"; fi
        printf '%s\n' "$gt2_error" >&2
        exit 1
    fi
fi

gt2_stage='Opening installation log'
gt2_log="$gt2_root/GT2-SteamDeck-install.log"
gt2_fail() {
    local status="$1"
    trap - ERR
    printf '\nINSTALLATION STOPPED: %s (error %s).\nYour discs and saves have not been removed.\nLog: %s\nYou can close this window.\n' "$gt2_stage" "$status" "$gt2_log" >&2
    exit "$status"
}
trap 'gt2_fail "$?"' ERR
touch "$gt2_log"
exec > >(tee -a "$gt2_log") 2>&1
printf '\nGT2 0.7.0 - Steam Deck installer\n%s\n\n' "$(date)"
echo 'Keep this window open. Downloads and compilation may take a while.'
echo 'The installer does not need a sudo password or changes to the SteamOS system partition.'

gt2_stage='Checking Steam Deck environment'
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
    echo 'This installer requires x86_64 Linux / Steam Deck Desktop Mode.'
    gt2_fail 1
fi
if ! command -v flatpak >/dev/null; then
    echo 'Flatpak is missing. SteamOS normally includes it; install Flatpak through your distribution first.'
    gt2_fail 1
fi
gt2_output="${XDG_CACHE_HOME:-$HOME/.cache}/gt2/steamdeck-build"
mkdir -p "$gt2_output"
gt2_output="$(cd "$gt2_output" && pwd)"
if command -v flock >/dev/null; then
    exec 9> "$gt2_output/install.lock"
    if ! flock -n 9; then
        echo 'Another GT2 installer is already running. Use its window or wait for it to finish.'
        gt2_fail 1
    fi
fi
gt2_bundle="${1:-$gt2_root/GT2-0.7.0-steamdeck.flatpak}"
if [ "$#" -gt 0 ] && [ ! -s "$gt2_bundle" ]; then
    echo "Package not found: $gt2_bundle"
    gt2_fail 1
fi

gt2_stage='Connecting to Flathub'
echo '[1/4] Preparing Flatpak...'
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
if [ ! -s "$gt2_bundle" ]; then
    gt2_stage='Installing build tools and graphics runtime'
    echo '[2/4] Installing the build tools and runtime. This download can be several GB.'
    flatpak install --user --arch=x86_64 --noninteractive --assumeyes flathub \
        org.flatpak.Builder org.freedesktop.Sdk//25.08 org.freedesktop.Platform//25.08
    gt2_stage='Compiling and checking GT2'
    echo '[3/4] Building GT2 and running its checks...'
    gt2_result="$(mktemp "$gt2_output/result.XXXXXX")"
    GT2_BUILD_RESULT_FILE="$gt2_result" /bin/bash "$gt2_root/BUILD-STEAMDECK.sh"
    IFS= read -r gt2_bundle < "$gt2_result"
    if [ ! -s "$gt2_bundle" ]; then
        echo 'The build did not produce a game package.'
        gt2_fail 1
    fi
else
    echo '[2/4] Using the included game package.'
    echo '[3/4] Compilation is not needed.'
fi

gt2_stage='Installing GT2'
echo '[4/4] Installing GT2...'
flatpak install --user --noninteractive --assumeyes "$gt2_bundle"
flatpak info --user io.github.gt2pc.GT2 >/dev/null

gt2_stage='Installing the desktop launcher'
gt2_launch_dir="${XDG_DATA_HOME:-$HOME/.local/share}/gt2-launcher"
mkdir -p "$gt2_launch_dir"
cp "$gt2_root/PLAY-STEAMDECK.sh" "$gt2_root/IMPORT-DISC-STEAMDECK.sh" "$gt2_launch_dir/"
chmod 755 "$gt2_launch_dir/PLAY-STEAMDECK.sh" "$gt2_launch_dir/IMPORT-DISC-STEAMDECK.sh"
gt2_launcher="$gt2_launch_dir/PLAY-STEAMDECK.sh"
gt2_exec="${gt2_launcher//\\/\\\\}"
gt2_exec="${gt2_exec//\"/\\\"}"
gt2_exec="${gt2_exec//\$/\\\$}"
gt2_exec="${gt2_exec//\`/\\\`}"
gt2_exec="${gt2_exec//%/%%}"

gt2_stage='Creating the desktop shortcut'
gt2_desktop="$(xdg-user-dir DESKTOP 2>/dev/null || true)"
if [ -z "$gt2_desktop" ]; then gt2_desktop="$HOME/Desktop"; fi
mkdir -p "$gt2_desktop"
cat > "$gt2_desktop/GT2.desktop" <<GT2_SHORTCUT
[Desktop Entry]
Type=Application
Name=GT2
Comment=Play GT2
Exec=/bin/bash "$gt2_exec"
Icon=io.github.gt2pc.GT2
Terminal=false
Categories=Game;SportsGame;
StartupNotify=false
GT2_SHORTCUT
chmod 755 "$gt2_desktop/GT2.desktop"
printf '\nGT2 INSTALLED\nOpen the GT2 desktop shortcut to choose your disc.\nRight-click GT2 and choose Add to Steam for Gaming Mode.\nLog: %s\nYou can close this window.\n' "$gt2_log"
if command -v kdialog >/dev/null; then
    kdialog --title 'GT2 installed' --msgbox 'GT2 is installed. Open the GT2 desktop shortcut to select your disc. Right-click GT2 and choose Add to Steam for Gaming Mode.' || true
fi
