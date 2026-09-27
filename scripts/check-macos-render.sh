#!/bin/bash
# Read the installed Arcade disc, compare rendering paths, keep test saves isolated.
set -uo pipefail
gt2_check() {
    if [ "$(uname -s)" != Darwin ]; then echo 'This check requires macOS.' >&2; return 1; fi
    local app='' candidate
    for candidate in "$HOME/Applications/GT2.app" /Applications/GT2.app; do
        if [ -x "$candidate/Contents/MacOS/gt2game" ]; then app="$candidate"; break; fi
    done
    if [ -z "$app" ]; then echo 'Installed GT2.app was not found.' >&2; return 1; fi
    if pgrep -x gt2game >/dev/null; then echo 'Close GT2 first, then run this check again.' >&2; return 1; fi
    local data="${GT2_DATA_ROOT:-$HOME/Library/Application Support/GT2}"
    local disc="$data/arcade/disc.raw2352"
    if [ ! -f "$disc" ]; then echo 'Import your Arcade disc in GT2.app first.' >&2; return 1; fi
    mkdir -p "$HOME/Downloads"
    local out
    out="$(mktemp -d "$HOME/Downloads/GT2-Mac-render.XXXXXX")" || return 1
    mkdir -p "$out/saves"
    cd "$out" || return 1
    local original_saves="${GT2_SAVE_ROOT:-$data/saves}"
    if [ -f "$original_saves/arcade/settings.txt" ]; then
        cp "$original_saves/arcade/settings.txt" "$out/saves/settings.txt" || return 1
    fi
    export GT2_SAVE_ROOT="$out/saves"
    export VK_DRIVER_FILES="$app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
    export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
    unset SDL_VULKAN_LIBRARY VK_ADD_DRIVER_FILES
    export MVK_CONFIG_LOG_LEVEL=3
    local game="$app/Contents/MacOS/gt2game" status=0
    local common=("$disc" --window 960x720 --no-sound --settings "$out/saves/settings.txt"
        --card "$out/saves/card1.mcd" --card2 "$out/saves/card2.mcd" --career "$out/saves/career.dat")
    echo 'Four short automatic checks will open. Do not press keys; each closes itself.'
    echo 'Capturing movie...'
    GT2_RENDER_CAPTURE="$out/movie.capture" "$game" "${common[@]}" --movie 24 \
        --shot-at 600 "$out/movie.png" --script '720:escape' > "$out/movie.log" 2>&1 || status=1
    echo 'Capturing Arcade car selection...'
    GT2_RENDER_CAPTURE="$out/arcade.capture" "$game" "${common[@]}" --title --no-movies \
        --script '60:enter,180:enter,300:enter,420:enter,540:enter,660:enter' --shot-at 810 "$out/arcade.png" \
        --arcade-frames 820 > "$out/arcade.log" 2>&1 || status=1
    echo 'Comparing car selection with the general shader...'
    GT2_CACHE_MENU_SHADER=0 "$game" "${common[@]}" --title --no-movies \
        --script '60:enter,180:enter,300:enter,420:enter,540:enter,660:enter' --shot-at 810 "$out/arcade-general-shader.png" \
        --arcade-frames 820 > "$out/arcade-general-shader.log" 2>&1 || status=1
    echo 'Capturing settings menu (the check sends F10 internally)...'
    GT2_RENDER_CAPTURE="$out/settings.capture" "$game" "${common[@]}" --title --no-movies \
        --script '60:f10,120:escape' --shot-at 90 "$out/settings.png" \
        --arcade-frames 150 > "$out/settings.log" 2>&1 || status=1
    sw_vers > "$out/system.txt"
    printf '\nCheck exit status: %s\n' "$status" >> "$out/system.txt"
    for candidate in "$out/"*.capture; do
        if [ -f "$candidate" ]; then gzip "$candidate" || status=1; fi
    done
    # Include only these test captures/logs, not the player's images, saves, or credentials.
    local files=() file
    for file in *.png *.log *.txt *.capture.gz; do
        if [ -f "$file" ]; then files+=("$file"); fi
    done
    /usr/bin/zip -q "$out.zip" "${files[@]}" || return 1
    /usr/bin/open -R "$out.zip"
    printf '\nSend this diagnostic archive: %s.zip\n' "$out"
    return "$status"
}
gt2_check
gt2_result=$?
if [ -t 0 ]; then printf '\nPress Enter to close.\n'; read -r _; fi
exit "$gt2_result"
