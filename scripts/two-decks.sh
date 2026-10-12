#!/bin/sh
# Two CDJ-2000 decks on one Pro DJ Link segment, in one guest time.
#
#   scripts/two-decks.sh OUT PORT_A PORT_B [CARD]
#
# Deck A (PORT_A..+5) loads row 0 of playlist row 4 and plays; deck B (PORT_B..+5) loads row 1 and stays
# cued.  Both machines are left running (--keep) for gdb probes (PORT+3) and panel_control (PORT+4); stop each
# by its own CDJ_LINK_PORT (never pkill qemu), and the hub by its --listen path.  The hub writes
# OUT/hub/link.pcap (Wireshark), events.jsonl and summary.json.  CDJ_QEMU picks the binary, CDJ_WATCH=HEX,HEX
# is inherited by both decks.  STOP_A / STOP_B (default: time / loaded) end deck A's / deck B's scenario after that step; HUB_SECONDS (default 86400) is the hub's life.  OUT must be a SHORT PATH RELATIVE to the repo root: the hub's unix socket lives
# under it and an absolute path under a deep home directory is "AF_UNIX path too long".
set -e
OUT=${1:?out dir (relative to the repo root)}; A=${2:?port A}; B=${3:?port B}
CARD=${4:-runs/cards/aconcert/card.img}
STOP_A=${STOP_A:-time}; STOP_B=${STOP_B:-loaded}
# the card's menu: ROOT_ROW / PLAYLIST_ROW (a rekordbox card has the playlists on root row 4; runs/cards/var-s3 is
# ROOT_ROW=4 PLAYLIST_ROW=0)
ROOT_ROW=${ROOT_ROW:-0}; PLAYLIST_ROW=${PLAYLIST_ROW:-4}
cd "$(dirname "$0")/.."
H=$OUT/hub
rm -rf "$H" "$OUT/a" "$OUT/b" "runs/tmp-$A" "runs/tmp-$B"
mkdir -p "$H"
# a copy-on-write clone where the file system has one: macOS `cp -c`, GNU `cp --reflink=auto` (cp -c is something else there)
if [ "$(uname)" = Darwin ]; then CLONE="cp -c"; else CLONE="cp --reflink=auto"; fi
$CLONE "$CARD" "$H/cardA.img"
$CLONE "$CARD" "$H/cardB.img"
# nohup and no deadline worth the name: the hub must outlive this script and a closed shell (it used to end by itself
# after 3600 s, and the decks then ran on alone: "Connection reset by peer").  Why it ended is in hub/events.jsonl.
nohup .venv/bin/python -m tools.cdj_main.link_hub --sync --decks 2 --seconds "${HUB_SECONDS:-86400}" \
    --listen "unix:$PWD/$H/hub.sock" "$H" > "$H/hub.out" 2>&1 &
echo $! > "$H/hub.pid"
sleep 2
.venv/bin/python -m tools.cdj_main.cosim_scenario --card "$H/cardA.img" --root-row "$ROOT_ROW" --playlist-row "$PLAYLIST_ROW" \
    --out "$OUT/a" --port "$A" --boot-arg="--link-hub=sync:unix:$PWD/$H/hub.sock" \
    --boot-arg=--link-mac=02:00:00:00:00:02 --stop-after "$STOP_A" --keep > "$OUT/a.out" 2>&1 &
PA=$!
.venv/bin/python -m tools.cdj_main.cosim_scenario --card "$H/cardB.img" --root-row "$ROOT_ROW" --playlist-row "$PLAYLIST_ROW" --track-row 1 \
    --out "$OUT/b" --port "$B" --boot-arg="--link-hub=sync:unix:$PWD/$H/hub.sock" \
    --boot-arg=--link-mac=02:00:00:00:00:03 --stop-after "$STOP_B" --keep > "$OUT/b.out" 2>&1 &
PB=$!
wait $PA $PB
grep -h -E "^[a-z0-9]+ +(ok|FAIL)" "$OUT/a.out" "$OUT/b.out"
echo "hub still running; decks kept on $A and $B"
