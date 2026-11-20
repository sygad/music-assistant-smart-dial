#!/bin/sh
# Build and serve the browser simulator (sim/): the knob's own screens in a web page.
#
# Like tools/fw.sh, this works on a local copy: the repo is on a network share,
# which is far too slow to build on.
#
#   tools/sim.sh build        compile into the local copy's sim/build/web
#   tools/sim.sh serve [port] build, then serve it on http://localhost:8360 (default), and to
#                             phones on the home Wi-Fi at the Mac's address (printed)
#   tools/sim.sh lvconf       refresh sim/lv_kconfig.h from the firmware's LVGL settings
#   tools/sim.sh data         copy the library and covers from Music Assistant (read-only)
#   tools/sim.sh recent       refresh only what was recently played (quick), then rebuild
#   tools/sim.sh demo [dir]   the public demo (a slice of the library, no server) into the
#                             story repo's demo/ folder
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${KNOB_SIM_WORK:-$HOME/esp/work/bathroom-knob-sim}"
FW_WORK="${KNOB_WORK:-$HOME/esp/work/bathroom-knob}"
LVGL_DIR="${LVGL_DIR:-$FW_WORK/firmware/managed_components/lvgl__lvgl}"
WEB="$WORK/sim/build/web"

build() {
    if [ ! -f "$LVGL_DIR/lvgl.h" ]; then
        echo "LVGL not found at $LVGL_DIR: build the firmware once (tools/fw.sh build) or set LVGL_DIR" >&2
        exit 1
    fi
    mkdir -p "$WORK"
    rsync -a --delete --exclude build/ --exclude data/ "$REPO/core" "$REPO/ui" "$REPO/sim" "$WORK/"
    emcmake cmake -S "$WORK/sim" -B "$WORK/sim/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DLVGL_DIR="$LVGL_DIR" >/dev/null
    cmake --build "$WORK/sim/build"
    mkdir -p "$WEB"
    cp "$WORK/sim/build/knob.js" "$WORK/sim/build/knob.wasm" "$WORK"/sim/web/* "$WEB/"
    if [ -d "$REPO/sim/data" ]; then
        rsync -a "$REPO/sim/data/" "$WEB/data/"
    else
        echo "No library copied yet: run tools/sim.sh data" >&2
    fi
    echo "Built $WEB"
}

case "$1" in
build) build ;;
serve)
    build
    PORT="${2:-8360}"
    echo "Open http://localhost:$PORT"
    # every address the Mac has (Wi-Fi and wired): if Wi-Fi devices can't see each
    # other (client isolation), a phone can still reach the wired one
    for IP in $(ifconfig | awk '/inet / && $2 !~ /^127\./ {print $2}'); do
        echo "On a phone on the home network: http://$IP:$PORT"
    done
    # the page, and Recent fetched live from Music Assistant (token stays on the Mac)
    exec python3 -I "$REPO/tools/sim_serve.py" "$WEB" "$PORT"
    ;;
lvconf)
    H="$FW_WORK/firmware/build/config/sdkconfig.h"
    {
        echo "/* LVGL settings for the browser simulator: the firmware's own LVGL settings,"
        echo " * copied from its generated sdkconfig.h (firmware/sdkconfig.defaults plus the"
        echo " * LVGL component's defaults), so screens draw exactly as on the knob."
        echo " * Regenerate after changing LVGL settings: tools/sim.sh lvconf */"
        echo "#pragma once"
        echo "#include <stdlib.h>"
        grep "^#define CONFIG_LV_" "$H"
        echo ""
        echo "/* Simulator only: a failed LVGL check stops with a message, not a silent hang */"
        echo "#define CONFIG_LV_ASSERT_HANDLER abort();"
    } >"$REPO/sim/lv_kconfig.h"
    echo "Wrote sim/lv_kconfig.h"
    ;;
data) exec python3 -I "$REPO/tools/sim_data.py" ;;
recent) python3 -I "$REPO/tools/sim_data.py" --recent && build ;;
demo)
    # the public demo: the same build with a slice of the library, no server needed
    # into the public repo's demo/: beside this script when this is that repo,
    # or the checkout of it kept inside the working repo
    OUT="${2:-$REPO/demo}"
    [ -z "${2:-}" ] && [ -d "$REPO/public/music-assistant-smart-dial-v2" ] && OUT="$REPO/public/music-assistant-smart-dial-v2/demo"
    build
    rm -rf "$OUT" && mkdir -p "$OUT"
    cp "$WEB/knob.js" "$WEB/knob.wasm" "$WEB/sim.js" "$WEB/sim.css" "$WEB/index.html" "$OUT/"
    python3 -I "$REPO/tools/sim_demo.py" "$REPO/sim/data" "$OUT/data" "$OUT/index.html"
    echo "Public demo in $OUT"
    ;;
*) sed -n '2,10p' "$0"; exit 1 ;;
esac
