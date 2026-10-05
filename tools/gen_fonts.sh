#!/bin/bash
# Regenerates the UI fonts in firmware/main/fonts from Inter (SIL OFL) and
# LVGL's FontAwesome subset. Needs node (npx) and network on first run. The
# generated .c files are committed, so normal builds do not need this.
# Pass font names (e.g. font_title_36) to regenerate only those.
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
work="${TMPDIR:-/tmp}/tab5-fonts"; mkdir -p "$work"
if [[ ! -f "$work/inter/extras/ttf/Inter-Regular.ttf" ]]; then
  curl -sSL -o "$work/inter.zip" https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
  unzip -o -q "$work/inter.zip" -d "$work/inter"
fi
ttf="$work/inter/extras/ttf"
fa="$project_root/firmware/managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
out="$project_root/firmware/main/fonts"
# Text: ASCII, Latin-1 Supplement, Latin Extended-A (accented artist names),
# typographic dashes/quotes/ellipsis, euro and trademark signs.
text="0x20-0x7E,0xA0-0x17F,0x2010-0x2027,0x2032-0x2033,0x20AC,0x2122"
# LVGL's LV_SYMBOL_* set plus star, link, layer-group and broadcast-tower.
syms="61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650,61445,61633,62973,62745"
wanted() { [[ ${#only[@]} -eq 0 ]] || [[ " ${only[*]} " == *" $1 "* ]]; }
only=("$@")
conv() { # name size ttf [with-symbols]
  wanted "$1" || return 0
  local args=(--bpp 4 --size "$2" --font "$ttf/$3" -r "$text")
  [[ "${4:-}" == syms ]] && args+=(--font "$fa" -r "$syms")
  npx -y lv_font_conv "${args[@]}" --format lvgl --no-compress --force-fast-kern-format --lv-include lvgl.h -o "$out/$1.c"
  echo "$1: $(wc -c < "$out/$1.c") bytes of C"
}
conv font_title_56 56 Inter-SemiBold.ttf
conv font_title_36 36 Inter-SemiBold.ttf
conv font_body_36 36 Inter-Medium.ttf
conv font_body_32 32 Inter-Medium.ttf syms
conv font_body_26 26 Inter-Regular.ttf syms
conv font_caption_22 22 Inter-Medium.ttf syms
if wanted font_icons_52; then
  npx -y lv_font_conv --bpp 4 --size 52 --font "$fa" -r "$syms" --format lvgl --no-compress --lv-include lvgl.h -o "$out/font_icons_52.c"
  echo "font_icons_52: $(wc -c < "$out/font_icons_52.c") bytes of C"
fi
cp "$work/inter/LICENSE.txt" "$out/Inter-LICENSE.txt"
