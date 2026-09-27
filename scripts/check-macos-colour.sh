#!/bin/bash
# Native render evidence: raw framebuffer colours and independent Metal A/Bs.
# Uses read-only disc data and a new disposable save directory for every run.
set -uo pipefail

gt2_colour_check() {
    if [ "$(uname -s)" != Darwin ]; then echo 'This check requires macOS.' >&2; return 1; fi
    local app='' candidate
    for candidate in "$HOME/Applications/GT2.app" /Applications/GT2.app; do
        if [ -x "$candidate/Contents/MacOS/gt2game" ]; then app="$candidate"; break; fi
    done
    if [ -z "$app" ]; then echo 'Installed GT2.app was not found.' >&2; return 1; fi
    if pgrep -x gt2game >/dev/null; then echo 'Close GT2 first, then run this check again.' >&2; return 1; fi
    local data="${GT2_DATA_ROOT:-$HOME/Library/Application Support/GT2}"
    data="$(cd "$data" && pwd -P)" || return 1
    local disc="$data/arcade/disc.raw2352"
    if [ ! -f "$disc" ]; then echo 'Import your Arcade disc in GT2.app first.' >&2; return 1; fi
    mkdir -p "$HOME/Downloads" || return 1
    local out
    out="$(mktemp -d "$HOME/Downloads/GT2-Mac-colour.XXXXXX")" || return 1
    cd "$out" || return 1
    export GT2_DATA_ROOT="$data"
    export VK_DRIVER_FILES="$app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
    export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
    unset SDL_VULKAN_LIBRARY VK_ADD_DRIVER_FILES GT2_MAC_LAUNCHER GT2_RENDER_CAPTURE
    local game="$app/Contents/MacOS/gt2game" status=0 name code
    local original_saves="${GT2_SAVE_ROOT:-$data/saves}"
    sw_vers > system.txt
    printf '\nGT2 colour and material diagnostics\n' >> system.txt
    shasum -a 256 "$game" >> system.txt
    printf 'Six automatic checks will run. Do not press keys or change fullscreen.\n'
    printf 'Each check closes itself. Your installed saves/settings are not changed.\n\n'
    for name in notice car-default car-general car-bindings car-precise car-combined; do
        mkdir -p "$out/saves/$name" || return 1
        if [ -f "$original_saves/arcade/settings.txt" ]; then
            cp "$original_saves/arcade/settings.txt" "$out/saves/$name/settings.txt" || return 1
        fi
        local save="$out/saves/$name"
        # Direct image launches read this per-run overlay, not the installed
        # shared settings. Keep optional BIOS movies out of the frame schedule.
        printf 'vr_ps1_intro=0\n' > "$save/settings.txt.overlay"
        if [ "$name" = notice ]; then printf 'hd_assets=0\n' >> "$save/settings.txt.overlay"; fi
        local common=("$disc" --window 960x720 --no-sound --settings "$save/settings.txt"
            --card "$save/card1.mcd" --card2 "$save/card2.mcd" --career "$save/career.dat")
        local run=(--title --no-movies --script '60:enter,180:enter,300:enter,420:enter,540:enter'
            --shot-at 620 "$out/$name.png" --arcade-frames 630)
        if [ "$name" = notice ]; then
            # Force the boot screens in an automated run; field 260 is at full
            # brightness, outside both fade ramps. Exit through the settings menu.
            run=(--title --movies --shot-at 260 "$out/$name.png"
                --script '360:escape,370:f10,380:up,390:enter,400:up,410:enter' --title-frames 440)
        fi
        echo "Capturing $name..."
        (
            export GT2_SAVE_ROOT="$save"
            export GT2_CACHE_MENU_TEXTURES=1 GT2_CACHE_MENU_SHADER=1
            export MVK_CONFIG_LOG_LEVEL=3 MVK_CONFIG_DEBUG=0
            export MVK_CONFIG_FAST_MATH_ENABLED=2 MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1
            unset MVK_CONFIG_SHADER_DUMP_DIR MVK_CONFIG_SHADER_LOG_ESTIMATED_GLSL
            case "$name" in
                car-general) export GT2_CACHE_MENU_SHADER=0 ;;
                car-bindings) export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0 ;;
                car-precise) export MVK_CONFIG_FAST_MATH_ENABLED=0 ;;
                car-combined) export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0 MVK_CONFIG_FAST_MATH_ENABLED=0 ;;
            esac
            if [ "$name" = car-default ]; then
                export GT2_RENDER_CAPTURE="$out/car-default.capture"
                export MVK_CONFIG_DEBUG=1 MVK_CONFIG_LOG_LEVEL=4
            fi
            printf 'case=%s menu_shader=%s argument_buffers=%s fast_math=%s\n' "$name" \
                "$GT2_CACHE_MENU_SHADER" "$MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS" "$MVK_CONFIG_FAST_MATH_ENABLED"
            "$game" "${common[@]}" "${run[@]}"
        ) > "$out/$name.log" 2>&1
        code=$?
        printf '%s exit=%s\n' "$name" "$code" >> system.txt
        if [ "$code" -ne 0 ] || [ ! -s "$out/$name.png" ]; then
            echo "$name did not complete; its log will be included." >&2
            status=1
        fi
    done
    if [ -f car-default.capture ]; then gzip car-default.capture || status=1; fi
    printf '\nCheck exit status: %s\n' "$status" >> system.txt
    # Explicitly exclude test saves, disc images, invitations and credentials.
    local files=() file
    for file in *.png *.log system.txt *.capture.gz; do
        if [ -f "$file" ]; then files+=("$file"); fi
    done
    /usr/bin/zip -q "$out.zip" "${files[@]}" || return 1
    /usr/bin/open -R "$out.zip"
    printf '\nSend this diagnostic archive: %s.zip\n' "$out"
    return "$status"
}

gt2_colour_check
gt2_result=$?
if [ -t 0 ]; then printf '\nPress Enter to close.\n'; read -r _; fi
exit "$gt2_result"
