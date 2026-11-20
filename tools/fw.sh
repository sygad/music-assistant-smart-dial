#!/bin/sh
# Build, flash or monitor the firmware.
#
# The repo lives on a network share (SMB), which is far too slow for an ESP-IDF
# build. This copies the sources to a local mirror and runs idf.py there.
#
#   tools/fw.sh build            compile
#   tools/fw.sh flash            compile and flash over USB-C
#   tools/fw.sh ota              compile and update the knob over Wi-Fi (no cable)
#   tools/fw.sh <any idf.py args>
#
# ota is the one to use now the knob is on the bathroom wall (2 Nov 2026). It
# builds, serves the new firmware off this Mac for as long as it takes to
# fetch, tells Home Assistant where it is and asks the knob to take it. The
# knob writes it to its spare flash slot and restarts into it; if that build
# cannot get back on Wi-Fi, its bootloader puts the old one back by itself.
#
# Where the firmware is served from matters, because the knob is on the IoT
# VLAN and that network is one-way: this Mac can reach the
# knob's network but nothing on the knob's network can reach back to this Mac
# on the main network. Proved on 2 Nov 2026 - the knob tried the Mac for eighteen
# seconds and got no answer at all. So the file has to sit on something the
# knob can already see, and Home Assistant is exactly that: same subnet, always
# on, and the knob talks to it every second anyway. Anything in Home
# Assistant's www folder is served at /local/<name>.
#
# That folder is reached over the Samba share add-on. Mount it once in Finder
# (Go -> Connect to Server -> smb://<Home Assistant>, pick "config", and let the
# Mac remember it) and this script finds it from then on. Set KNOB_HA_WWW if it
# lands somewhere other than /Volumes/config/www.
#
# Nothing about the address is baked into the firmware - the knob reads it from
# input_text.bathroom_dial_firmware_url at the moment of the update - so the
# file can be moved anywhere later and the knob will follow without ever
# needing a cable again.
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${KNOB_WORK:-$HOME/esp/work/bathroom-knob}"
PORT="${KNOB_PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"

mkdir -p "$WORK"
rsync -a --delete \
    --exclude build/ --exclude managed_components/ --exclude dependencies.lock \
    --exclude sdkconfig --exclude sdkconfig.old \
    "$REPO/core" "$REPO/ui" "$REPO/firmware" "$REPO/config" "$WORK/"

# The firmware version Settings shows, worked out rather than typed: the
# release is the latest vMAJOR.MINOR tag and the patch counts the commits since
# it, so v0.9 plus three commits is 0.9.3. Tag the next release to move it on.
TAG="$(git -C "$REPO" describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null || true)"
if [ -n "$TAG" ]; then
    PROJECT_VER="${TAG#v}.$(git -C "$REPO" rev-list --count "$TAG"..HEAD)"
else
    PROJECT_VER="0.0.$(git -C "$REPO" rev-list --count HEAD 2>/dev/null || echo 0)"
fi
export PROJECT_VER

# CMake only reads PROJECT_VER when it configures, so a build after a commit
# kept the old number: the knob said 0.9.0 with 0.9.8 on it (28 Oct 2026).
# Reconfigure when it has moved on, and only then.
CACHE="$WORK/firmware/build/CMakeCache.txt"
if [ -f "$CACHE" ] && ! grep -q "^PROJECT_VER:.*=$PROJECT_VER\$" "$CACHE"; then
    NEEDS_RECONFIGURE=1
fi

# A changed sdkconfig.defaults has to actually reach the build. The mirror
# keeps its generated sdkconfig (it is excluded from the rsync above, so a
# build does not re-run Kconfig every time), and that file wins over the
# defaults once it exists - so a new default is silently ignored. That is not
# a theoretical problem: it is how the knob was flashed on 2 Nov 2026 with
# bootloader rollback believed to be on and actually off, which is the one
# thing standing between a bad over-the-air build and a trip up the stairs.
# Dropping the generated file when the defaults are newer makes the build
# re-read them.
if [ "$REPO/firmware/sdkconfig.defaults" -nt "$WORK/firmware/sdkconfig" ]; then
    echo "sdkconfig.defaults has changed: re-reading it"
    rm -f "$WORK/firmware/sdkconfig"
    NEEDS_RECONFIGURE=1
fi

. "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
cd "$WORK/firmware"
export IDF_CCACHE_ENABLE=1
[ -n "${NEEDS_RECONFIGURE:-}" ] && idf.py reconfigure >/dev/null

if [ "${1:-}" = "ota" ]; then
    if [ -z "${HA_URL:-}" ]; then
        HA_URL="$(sed -n 's/^HA_SERVER=//p' "$REPO/config/secrets.local" | head -1)"
    fi
    : "${KNOB_HA_WWW:=/Volumes/config/www}"
    if [ -z "${HA_TOKEN:-}" ]; then
        HA_TOKEN="$(sed -n 's/^HA_TOKEN=//p' "$REPO/config/secrets.local" | head -1)"
    fi

    # Everything this needs, checked before anything is built, and each one
    # named so a failure is a sentence rather than a mystery. There are only
    # two requirements and neither of them is which network this Mac is on:
    # the knob fetches from Home Assistant, and so does this script. Being on
    # the same Wi-Fi as the knob is NOT required - and is actively unhelpful,
    # since the IoT network cannot reach the NAS this repo lives on (2 Nov
    # 2026: it unmounted the project mid-session).
    fail=""
    [ -n "$HA_TOKEN" ] || fail="$fail
  - No HA_TOKEN in config/secrets.local."

    CODE="$(curl -s -o /dev/null -m 8 -w '%{http_code}' \
            -H "Authorization: Bearer ${HA_TOKEN:-none}" "$HA_URL/api/" || true)"
    case "$CODE" in
    200) ;;
    000) fail="$fail
  - Cannot reach Home Assistant at $HA_URL.
    This Mac needs a route to it - any network that reaches it will do." ;;
    401|403) fail="$fail
  - Home Assistant refused the token (HTTP $CODE). Check HA_TOKEN in
    config/secrets.local is still valid." ;;
    *) fail="$fail
  - Home Assistant answered HTTP $CODE at $HA_URL/api/, which is not 200." ;;
    esac

    if [ ! -d "$KNOB_HA_WWW" ]; then
        fail="$fail
  - Home Assistant's www folder is not mounted at $KNOB_HA_WWW.
    Mount its config share once and the Mac will remember it:
      Finder -> Go -> Connect to Server -> smb://<Home Assistant> -> config
    If it mounts elsewhere, set KNOB_HA_WWW to its www folder."
    elif ! touch "$KNOB_HA_WWW/.knob-write-test" 2>/dev/null; then
        fail="$fail
  - $KNOB_HA_WWW is mounted but not writable."
    else
        rm -f "$KNOB_HA_WWW/.knob-write-test"
    fi

    if [ -n "$fail" ]; then
        printf 'Cannot update over the air:\n%s\n' "$fail" >&2
        exit 1
    fi

    idf.py build
    BIN="$WORK/firmware/build/bathroom_knob.bin"
    [ -f "$BIN" ] || { echo "No $BIN after the build" >&2; exit 1; }
    SIZE="$(wc -c < "$BIN" | tr -d ' ')"

    # Written beside the live name and moved into place, so a knob that asks
    # mid-copy can never be handed half a file.
    cp "$BIN" "$KNOB_HA_WWW/.bathroom-knob.bin.part"
    mv "$KNOB_HA_WWW/.bathroom-knob.bin.part" "$KNOB_HA_WWW/bathroom-knob.bin"
    URL="$HA_URL/local/bathroom-knob.bin"

    # Ask Home Assistant for it back before asking the knob to. This is the
    # only check that proves what the knob will actually get: the share can be
    # mounted and writable and still be the wrong folder, in which case the
    # file is safely written somewhere nothing serves it.
    SERVED="$(curl -s -o /dev/null -m 15 -w '%{http_code} %{size_download}' "$URL" || true)"
    if [ "$SERVED" != "200 $SIZE" ]; then
        cat >&2 <<MSG
Home Assistant is not serving what was just written.
  asked for: $URL
  expected:  200 $SIZE bytes
  got:       $SERVED
$KNOB_HA_WWW is probably not the www folder Home Assistant serves at /local.
MSG
        exit 1
    fi
    echo "Home Assistant is serving $SIZE bytes at $URL"

    ha() {  # ha <method> <path> [body]
        curl -sS -X "$1" -H "Authorization: Bearer $HA_TOKEN" -H "Content-Type: application/json" \
             ${3:+-d "$3"} "$HA_URL/api/$2"
    }
    ha POST "services/input_text/set_value" \
        "{\"entity_id\":\"input_text.bathroom_dial_firmware_url\",\"value\":\"$URL\"}" >/dev/null
    # What the knob says now, and WHEN it last said it. The timestamp is what
    # proves an update landed: the version string on its own cannot, because
    # re-flashing the same build - which is most of testing - would look
    # identical to nothing having happened at all.
    # What the knob last said about updating, so a result left over from a
    # previous attempt is not read as this one's. Without this, asking again
    # after any failure reports that same failure instantly and gives up
    # before the knob has even noticed it was asked (2 Nov 2026).
    # ---- Is the dial actually there to be asked? (6 Nov 2026) -----------------
    # It used to just ask and then wait five minutes to find out. Three states
    # are worth telling apart before spending that:
    #
    #   awake or light asleep  - on the network, polling every ten seconds; ask
    #                            it and it answers within one poll.
    #   deep asleep            - off the network entirely. NOTHING can reach it
    #                            and nothing can wake it but a turn of the knob
    #                            or its own six-hourly check-in. There is no
    #                            remote wake to be had: the radio is off.
    #   gone                   - flat, or crashed, or off the air.
    #
    # The UPTIME sensor is the liveness test, and it has to be the uptime
    # (7 Nov 2026). This was the charge sensor, and it told Darren's dial - lit,
    # in his hand, being used - that it was in deep sleep, and refused to
    # publish. The charge is wrong twice over:
    #
    #   - it is only measured with the screen OFF (battery_report in
    #     app_main.c: a loaded rail says nothing about the cell), so a dial
    #     somebody is using never posts one at all;
    #   - Home Assistant records nothing when a state is posted with the value
    #     and attributes it already had - the same trap this file's wait loop
    #     carries a comment about - so a charge sitting on 50% leaves no mark
    #     however often it is sent.
    #
    # The uptime has neither fault: it is a different number every time it is
    # posted, so Home Assistant always records it, and the dial posts it on its
    # own five minutes whether or not there is a charge figure (say_vitals in
    # ha.c, from 0.9.156). Needs that firmware or newer to mean anything, which
    # is the other reason nothing below REFUSES any more.
    LAST="$(ha GET "states/sensor.bathroom_dial_uptime" | sed -n 's/.*"last_reported":"\([^"]*\)".*/\1/p')"
    AGE=""
    if [ -n "$LAST" ]; then
        AGE="$(python3 - "$LAST" <<'PYAGE'
import datetime, sys
t = datetime.datetime.fromisoformat(sys.argv[1])
print(int((datetime.datetime.now(datetime.timezone.utc) - t).total_seconds()))
PYAGE
)"
    fi
    # What the knob says now, taken BEFORE the switch is armed: the wait loop
    # below looks for something NEW, so these have to be read while the dial
    # still has nothing to say about this attempt.
    SAYS_WAS="$(ha GET "states/sensor.bathroom_dial_update" | sed -n 's/.*"state":"\([^"]*\)".*/\1/p')"
    BEFORE="$(ha GET "states/sensor.bathroom_dial_firmware")"
    WAS="$(printf '%s' "$BEFORE" | sed -n 's/.*"state":"\([^"]*\)".*/\1/p')"
    # last_reported, not last_updated: Home Assistant leaves last_updated
    # alone when a state is set to the value it already had, and re-flashing
    # the same version is most of testing - so the one update that mattered
    # looked like nothing at all (2 Nov 2026). last_reported moves every time
    # the knob says who it is, which is exactly the signal wanted.
    WAS_AT="$(printf '%s' "$BEFORE" | sed -n 's/.*"last_reported":"\([^"]*\)".*/\1/p')"
    echo "On the knob now: ${WAS:-unknown}. Asking it to update to $PROJECT_VER..."

    # Nothing here refuses to publish any more (7 Nov 2026). It used to exit
    # before arming the switch and then say "the firmware and the URL are
    # staged, so one of those two will take it", which was not true: the URL
    # was set and the switch was not, so the dial had nothing to act on and the
    # update was never going to land at all. Whatever this test believes, the
    # request is armed; all that changes is whether it is worth waiting here.
    ARM="$(ha POST "services/input_boolean/turn_on" \
        "{\"entity_id\":\"input_boolean.bathroom_dial_update\"}")"
    ARMED="$(ha GET "states/input_boolean.bathroom_dial_update" | sed -n 's/.*"state":"\([^"]*\)".*/\1/p')"
    if [ "$ARMED" != "on" ]; then
        echo "Home Assistant did not arm input_boolean.bathroom_dial_update (it reads \"${ARMED:-nothing}\")." >&2
        printf '%s\n' "$ARM" >&2
        exit 1
    fi
    if [ -z "$AGE" ]; then
        cat <<MSG
The dial has never posted an uptime, so there is nothing here to say whether it
is on the network. It may be running a firmware older than 0.9.156, which only
posted one while asleep.

The update is armed either way: it lands the moment the dial next looks.
MSG
    elif [ "$AGE" -gt 420 ]; then
        cat <<MSG
Nothing has come from the dial for $((AGE / 60)) minutes. From 0.9.156 on it says
its uptime every five minutes whenever it is on the network, awake or asleep, so
that most likely means deep sleep, which turns the radio off. It cannot be woken
from here: only a turn of the knob, or its own check-in.

The update is armed, so one of those two will take it.
  - To have it now:   give the knob a turn, then run this again.
  - To let it happen:  nothing. It lands at the next check-in, within 6 hours.
MSG
        exit 2
    else
        echo "The dial is up (it spoke $AGE s ago) and will hear this within a poll."
    fi

    # Wait for the knob to come back running the new firmware. It polls every
    # ten seconds when its screen is off, then has to download and restart.
    for i in $(seq 1 60); do
        sleep 5
        SAYS="$(ha GET "states/sensor.bathroom_dial_update" | sed -n 's/.*"state":"\([^"]*\)".*/\1/p')"
        AFTER="$(ha GET "states/sensor.bathroom_dial_firmware")"
        NOW="$(printf '%s' "$AFTER" | sed -n 's/.*"state":"\([^"]*\)".*/\1/p')"
        NOW_AT="$(printf '%s' "$AFTER" | sed -n 's/.*"last_reported":"\([^"]*\)".*/\1/p')"
        printf '\r  %-44s firmware %s' "${SAYS:-?}" "${NOW:-?}"
        # Only once it has said something new about this attempt. Since 6 Nov
        # 2026 the firmware puts the attempt number in the text, so two
        # identical failures in a row are two different states and this can
        # never again sit through a stale one for five minutes.
        if [ "$SAYS" != "$SAYS_WAS" ]; then
            case "$SAYS" in failed:*) echo; echo "The knob refused it: $SAYS" >&2; exit 1;; esac
        fi
        # The one unambiguous signal that it landed: the knob reached
        # "installing" and then said "idle", which only happens on the far
        # side of a restart. Timestamps cannot be used - Home Assistant
        # records nothing when a state is re-posted unchanged, so a reflash of
        # the same version leaves no trace at all (2 Nov 2026).
        case "$SAYS" in installing|*%) INSTALLED=1;; esac
        if [ -n "${INSTALLED:-}" ] && [ "$SAYS" = "idle" ]; then
            echo; echo "Done: the knob restarted and is running $NOW"
            exit 0
        fi
    done
    echo
    echo "Gave up waiting. The knob says: ${SAYS:-nothing}; firmware ${NOW:-unknown}" >&2
    # Since 4 Nov 2026 the dial drops into deep sleep after a couple of hours
    # untouched, and deep sleep takes it off the network entirely - it cannot be
    # asked anything until it next wakes. The file and the URL are already in
    # place, so the update will land by itself at the dial's next check-in; or
    # turn the knob and run this again to have it now.
    echo "If it has been untouched for hours it is in deep sleep and off the network." >&2
    echo "The firmware is served and the switch is armed, so it lands by itself at the" >&2
    echo "dial's next look - unless the dial took the switch and then failed, which" >&2
    echo "sensor.bathroom_dial_update would say. Or turn the knob and run this again." >&2
    exit 1
fi

if [ -n "$PORT" ]; then
    exec idf.py -p "$PORT" "$@"
else
    exec idf.py "$@"
fi
