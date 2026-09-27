#!/bin/bash
set -u
gt2_error() {
    printf '%s\n' "$1" >&2
    if command -v kdialog >/dev/null; then kdialog --title GT2 --error "$1"; fi
    exit 1
}
command -v flatpak >/dev/null || gt2_error 'Install GT2 in Steam Deck Desktop Mode first.'
gt2_image=''
gt2_choose=false
if [ "${1:-}" = --import ]; then
    gt2_choose=true
    shift
fi
gt2_picker() {
    command -v kdialog >/dev/null || gt2_error 'Open GT2 in Desktop Mode to import a disc.'
    gt2_image="$(kdialog --title 'GT2 - select disc image' --getopenfilename "$HOME" '*.bin *.BIN *.iso *.ISO|PlayStation disc image (*.bin, *.iso)')"
    gt2_status="$?"
    if [ "$gt2_status" -eq 1 ]; then return 1; fi
    [ "$gt2_status" -eq 0 ] || gt2_error 'The file picker failed to return a disc image.'
    [ -n "$gt2_image" ] || gt2_error 'The file picker returned an empty path.'
    case "$gt2_image" in /*) ;; *) gt2_error 'The file picker did not return a local absolute path.' ;; esac
    [ -f "$gt2_image" ] && [ -r "$gt2_image" ] || gt2_error 'The selected disc cannot be read. Check that its drive is connected.'
}
while :; do
    if "$gt2_choose"; then
        gt2_picker || exit 0
    fi
    gt2_options=(--user --env=GT2_HOST_DISC_PICKER=1 --command=gt2launcher)
    gt2_import=()
    if [ -n "$gt2_image" ]; then
        gt2_options+=("--filesystem=$gt2_image:ro")
        gt2_import=(--import "$gt2_image")
    fi
    flatpak run "${gt2_options[@]}" io.github.gt2pc.GT2 "${gt2_import[@]}" "$@"
    gt2_status="$?"
    if [ "$gt2_status" -eq 42 ]; then
        gt2_choose=true
        gt2_image=''
        continue
    fi
    [ "$gt2_status" -eq 0 ] || gt2_error "GT2 returned error $gt2_status. See ~/.var/app/io.github.gt2pc.GT2/data/GT2/gt2game.log."
    exit 0
done
