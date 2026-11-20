#!/bin/sh
# Regenerate ui/fonts/* from Inter. The exact version matters: Inter 4.1 with
# lv_font_conv 1.5.3 reproduces the committed files byte for byte, and any other
# pairing redraws every glyph slightly differently.
#
#   tools/fonts.sh /path/to/Inter-4.1/extras/ttf
#
# The text ranges are Latin-1 and Latin Extended-A (accented names), the dashes
# and quotes Music Assistant titles use, the ellipsis, and U+20AC "€" - artists
# style their names with it ("¥øu$uk€ ¥uk1mat$u"), and without the glyph the
# screen draws a box. The two extralight display fonts carry only what they
# draw: digits, a colon and the hyphen of the clock's "--:--" before the time
# is known, and "#" and A-Z for the album letter.
set -e
TTF="${1:?usage: tools/fonts.sh /path/to/Inter/extras/ttf}"
OUT="$(cd "$(dirname "$0")/../ui/fonts" && pwd)"
CONV="${LV_FONT_CONV:-lv_font_conv}"
TEXT="-r 0x20-0x7E -r 0xA0-0x17F -r 0x2013-0x2014 -r 0x2018-0x201D -r 0x2026 -r 0x20AC"

gen() {  # weight size ranges
    name="font_inter_$1_$2"
    cap="$(printf '%s' "$1" | awk '{print toupper(substr($0,1,1)) substr($0,2)}')"
    case "$1" in extralight) cap=ExtraLight ;; esac
    # shellcheck disable=SC2086
    $CONV --no-compress --bpp 4 --size "$2" --font "$TTF/Inter-$cap.ttf" $3 \
        --format lvgl --lv-include lvgl.h -o "$OUT/$name.c" --lv-font-name "$name"
}

for s in 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 26; do gen regular "$s" "$TEXT"; done
for s in 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36; do gen light "$s" "$TEXT"; done
gen extralight 96  "-r 0x20,0x2D,0x30-0x3A"
gen extralight 160 "-r 0x23,0x41-0x5A"

# The converter writes whatever paths it was given into each file's header, so
# normalise them to bare filenames: the recipe stays readable and a rerun from a
# different directory doesn't show up as a change to every font.
sed -i '' -E -e 's|(--font )[^ ]*/(Inter-[A-Za-z]+\.ttf)|\1\2|' \
             -e 's|(-o )[^ ]*/(font_inter_[a-z0-9_]+\.c)|\1\2|' "$OUT"/font_inter_*.c
