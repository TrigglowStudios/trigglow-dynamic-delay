#!/usr/bin/env bash
# One relay_harness run: ffmpeg as the platform (RTMP listener writing FLV),
# relay_harness with TMP/TEMP pointed at the drive under test, ffmpeg -re
# publishing a pre-encoded FLV (stream copy) as OBS.
#
#   run.sh NAME SRC TMPDIR OUTDIR SCRIPT KEEP MAX SPILLMB PORT
set -u
NAME=$1 SRC=$2 TMPD=$3 OUTD=$4 SCRIPT=$5 KEEP=$6 MAX=$7 SPILL=$8 PORT=$9
H=/e/Conding/Projects/obs-dd-bench/build_x64/tests/RelWithDebInfo/relay_harness.exe
L=/e/dd-bench-work/logs
mkdir -p "$L"
ffmpeg -hide_banner -loglevel warning -y -listen 1 -f flv -i "rtmp://127.0.0.1:$PORT/live/k" \
	-map 0 -c copy -f flv "$OUTD/$NAME.flv" > "$L/$NAME.listener.log" 2>&1 &
LPID=$!
sleep 2
TMP="$TMPD" TEMP="$TMPD" "$H" "rtmp://127.0.0.1:$PORT/live" k "$SCRIPT" "$KEEP" "$MAX" "$SPILL" > "$L/$NAME.log" 2>&1 &
HPID=$!
for i in $(seq 1 100); do
	grep -q '^PUBLISH' "$L/$NAME.log" 2>/dev/null && break
	sleep 0.1
done
read -r _ URL KEY < <(grep '^PUBLISH' "$L/$NAME.log" | head -1)
echo "$(date +%T) $NAME start publish $URL" >> "$L/_runs.txt"
ffmpeg -hide_banner -loglevel warning -re -i "$SRC" -map 0 -c copy -f flv "$URL/$KEY" > "$L/$NAME.pub.log" 2>&1
echo "$(date +%T) $NAME publisher ended rc=$?" >> "$L/_runs.txt"
wait $HPID
echo "$(date +%T) $NAME harness ended rc=$?" >> "$L/_runs.txt"
wait $LPID
echo "$(date +%T) $NAME listener ended rc=$?" >> "$L/_runs.txt"
