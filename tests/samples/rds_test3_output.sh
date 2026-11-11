#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Usage: rds_test3_output.sh <rds-test3> <source dir> <work dir> [infer] [tracker] [osd]
# rds-test3 --output end to end on the street clip (one source, YOLO11n, the NvSORT tracker, tiler, OSD): H.264 into
# an .mp4 and H.265 into an .mkv, the whole clip; then H.265 into an .mp4 stopped by SIGINT partway. Each file is
# checked with gst-discoverer-1.0 (the container and codec, the duration) and decoded with rdsvideodec, counting the
# frames: the whole clip's file has as many frames as the clip (decoded the same way) and lasts as long; the
# interrupted file has one frame per count line printed and is finished (its duration known: the mp4's moov written).
# Prints the frame counts, durations and file sizes. Skips (77) as rds_test3_run.sh does, and without the VA H.264 and
# H.265 encoders. Logs and files are kept in <work dir>/rds-test3-output/. The clip's path goes into a file:// URI
# unescaped: source trees whose path has spaces or '%' are not supported.
set -eu
app=$1 src=$2 work=$3

clip=$src/.cache/media/crosswalk-720p.h264
# the clip's frame rate (24000/1001): one frame lasts 1001/24000 s
FRAME_NS=41708333

if [ "${4:-}" != infer ] || [ "${5:-}" != tracker ] || [ "${6:-}" != osd ]; then
  echo 'built without rdsinfer, rdstracker or rdsosd: skipped'
  exit 77
fi
if [ ! -e /dev/kfd ]; then
  echo 'no ROCm device (/dev/kfd): skipped'
  exit 77
fi
if [ ! -f "$src/.cache/models/yolo11n.onnx" ]; then
  echo 'YOLO11n not found: run tests/models/fetch-yolo11n.sh; skipped'
  exit 77
fi
if [ ! -f "$clip" ]; then
  echo 'street clip not found: run tests/models/fetch-tracker-clip.sh; skipped'
  exit 77
fi
for e in vah264enc vah265enc; do
  if ! gst-inspect-1.0 "$e" >/dev/null 2>&1; then
    echo "no $e (GStreamer's va plugin, or the VA driver can't encode): skipped"
    exit 77
  fi
done

fail() {
  echo "FAIL: $*"
  exit 1
}

dir=$work/rds-test3-output
rm -rf "$dir"
mkdir -p "$dir"
app_pid=
cleanup() {
  [ -z "$app_pid" ] || kill -KILL "$app_pid" 2>/dev/null || true
}
trap cleanup EXIT

# frames decoded by rdsvideodec from: <parser> <demuxer or ''> <file>; gst-launch-1.0's output goes to
# <work dir>/rds-test3-output/<file name>.decode.log
count_frames() {
  local demux=${2:+$2 !}
  # shellcheck disable=SC2086
  gst-launch-1.0 -v filesrc location="$3" ! $demux "$1" ! rdsvideodec ! fakesink silent=false sync=false 2>&1 |
    tee "$dir/$(basename "$3").decode.log" | grep -c 'last-message = chain' || true
}

# the duration gst-discoverer-1.0 reports, in nanoseconds; empty when it has none
discovered_ns() {
  sed -n 's/^ *Duration: \([0-9]*\):\([0-9]*\):\([0-9]*\)\.\([0-9]*\)$/\1 \2 \3 \4/p' "$1" | head -1 |
    awk '{ printf "%d\n", (($1 * 60 + $2) * 60 + $3) * 1000000000 + substr($4 "000000000", 1, 9) }'
}

# checks <file> (<container caps name> <codec caps name>) against <frames>: the decoded count and the duration within
# one frame; prints a summary line
check_file() {
  local file=$1 container=$2 codec=$3 want=$4 demux=$5 parser=$6 info frames ns size
  [ -s "$file" ] || fail "$file missing or empty"
  info=$dir/$(basename "$file").discoverer
  gst-discoverer-1.0 -v "$file" >"$info" 2>&1 || fail "gst-discoverer-1.0 failed on $file (see $info)"
  grep -q "container #0: $container\$" "$info" || fail "$file: not $container"
  grep -q "video #[0-9]*: $codec," "$info" || fail "$file: no $codec video"
  ns=$(discovered_ns "$info")
  [ -n "$ns" ] || fail "$file: no duration"
  frames=$(count_frames "$parser" "$demux" "$file")
  size=$(stat -c %s "$file")
  awk -v f="$(basename "$file")" -v n="$frames" -v ns="$ns" -v s="$size" 'BEGIN {
    printf "%s: %d frames, %.3f s, %d bytes (%.2f Mbit/s)\n", f, n, ns / 1e9, s, s * 8 / (ns / 1e9) / 1e6 }'
  [ "$frames" -eq "$want" ] ||
    fail "$file: $frames frames decoded, expected $want (see $dir/$(basename "$file").decode.log)"
  awk -v ns="$ns" -v want="$((want * FRAME_NS))" -v fr=$FRAME_NS \
    'BEGIN { d = ns - want; if (d < 0) d = -d; exit !(d <= fr) }' ||
    fail "$file: lasts $ns ns, expected $((want * FRAME_NS)) (+/- one frame)"
}

clip_frames=$(count_frames h264parse '' "$clip")
echo "street clip: $clip_frames frames decoded"
[ "$clip_frames" -gt 0 ] || fail "the clip decoded to no frames (see $dir/$(basename "$clip").decode.log)"

tracker=$src/samples/tracker/config_tracker_NvSORT.yml
# <file> <codec>: the whole clip
run_whole() {
  local log=$dir/$(basename "$1").log status=0
  "$app" --output "$1" --codec "$2" --tracker-config "$tracker" "file://$clip" >"$log" 2>&1 || status=$?
  grep -v '^source ' "$log" || true
  [ "$status" -eq 0 ] || fail "rds-test3 --output $1 exited with $status"
  [ "$(grep -c '^source 0 frame ' "$log" || true)" -eq "$clip_frames" ] || fail "$1: count lines missing"
}
run_whole "$dir/h264.mp4" h264
check_file "$dir/h264.mp4" video/quicktime video/x-h264 "$clip_frames" qtdemux h264parse
run_whole "$dir/h265.mkv" h265
check_file "$dir/h265.mkv" video/x-matroska video/x-h265 "$clip_frames" matroskademux h265parse

# SIGINT partway: the EOS finishes the file
file=$dir/h265-sigint.mp4
log=$dir/$(basename "$file").log
"$app" --output "$file" --codec h265 --tracker-config "$tracker" "file://$clip" >"$log" 2>&1 &
app_pid=$!
# rds-test3's stdout is a file here, so block-buffered: the count lines reach the log in chunks, and "frame 300" shows
# up only some frames after the pipeline passed it. That is fine for the wait (SIGINT just comes a little later), but
# the log is complete only once the app has exited and flushed it, so `printed` is counted after the wait below.
for _ in $(seq 1200); do
  grep -q '^source 0 frame 300:' "$log" && break
  kill -0 $app_pid 2>/dev/null || fail 'rds-test3 ended before frame 300'
  sleep 0.1
done
grep -q '^source 0 frame 300:' "$log" || fail 'no frame 300 within 120 s'
kill -INT $app_pid
for _ in $(seq 150); do
  kill -0 $app_pid 2>/dev/null || break
  sleep 0.1
done
if kill -0 $app_pid 2>/dev/null; then
  fail 'rds-test3 still running 15 s after SIGINT'
fi
status=0
wait $app_pid || status=$?
app_pid=
grep -v '^source ' "$log" || true
[ "$status" -eq 0 ] || fail "rds-test3 exited with $status after SIGINT"
grep -qx 'End of stream' "$log" || fail 'no end of stream after SIGINT'
printed=$(grep -c '^source 0 frame ' "$log" || true)
[ "$printed" -lt "$clip_frames" ] || fail 'SIGINT came after the whole clip'
check_file "$file" video/quicktime video/x-h265 "$printed" qtdemux h265parse
echo 'ok'
