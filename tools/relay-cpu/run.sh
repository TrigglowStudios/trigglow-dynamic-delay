#!/usr/bin/env bash
# One relay_harness run with a publisher standing in for OBS and a listener
# standing in for the platform (docs/STREAM_MODE.md, "CPU").
#
#   run.sh NAME SRC.flv SCRIPT KEEP MAX SPILLMB PORT HARNESS_DIR PEERS
#
# PEERS: "obs4096" (rtmp_peers.py publisher like OBS, platform announcing
# 4096-byte chunks: Twitch, Kick, Facebook), "obs128" (same publisher,
# platform announcing nothing: 128-byte chunks, like YouTube), or "ffmpeg"
# (ffmpeg -re as OBS and ffmpeg -listen as the platform: 128-byte chunks both
# ways). SCRIPT, KEEP, MAX, SPILLMB: relay_harness's arguments. Work files go to
# $WORK (default /e/dd-cpu-work): logs/NAME.log (the harness's status lines),
# out/NAME.flv (what the platform received).
NAME=$1 SRC=$2 SCRIPT=$3 KEEP=$4 MAX=$5 SPILL=$6 PORT=$7 HD=$8 PEERS=$9
W=${WORK:-/e/dd-cpu-work}
L=$W/logs O=$W/out
P=$(cd "$(dirname "$0")" && pwd)/rtmp_peers.py
mkdir -p "$L" "$O" "$W/tmp"
if [ "$PEERS" = ffmpeg ]; then
	ffmpeg -hide_banner -loglevel warning -y -listen 1 -f flv -i "rtmp://127.0.0.1:$PORT/live/k" \
		-map 0 -c copy -f flv "$O/$NAME.flv" > "$L/$NAME.listener.log" 2>&1 &
	sleep 2
else
	python -I "$P" sink "$PORT" "$O/$NAME.flv" "${PEERS#obs}" > "$L/$NAME.listener.log" 2>&1 &
	sleep 1
fi
LPID=$!
TMP="$(cygpath -w "$W/tmp" 2>/dev/null || echo "$W/tmp")"
TMP="$TMP" TEMP="$TMP" "$HD/relay_harness" "rtmp://127.0.0.1:$PORT/live" k "$SCRIPT" "$KEEP" "$MAX" "$SPILL" \
	> "$L/$NAME.log" 2>&1 &
HPID=$!
for _ in $(seq 1 100); do grep -q '^PUBLISH' "$L/$NAME.log" 2>/dev/null && break; sleep 0.1; done
read -r _ URL KEY < <(grep '^PUBLISH' "$L/$NAME.log" | head -1)
if [ "$PEERS" = ffmpeg ]; then
	ffmpeg -hide_banner -loglevel warning -re -i "$SRC" -map 0 -c copy -f flv "$URL/$KEY" > "$L/$NAME.pub.log" 2>&1
else
	python -I "$P" publish "$URL" "$KEY" "$SRC" > "$L/$NAME.pub.log" 2>&1
fi
wait $HPID
wait $LPID
echo "$NAME done"
