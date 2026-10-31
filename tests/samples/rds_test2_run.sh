#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Usage: rds_test2_run.sh <rds-test2> <source dir> <work dir> [infer] [tracker] [osd]
# rds-test2 end to end with its default configs: four copies of the street clip, YOLO11n, the NvSORT tracker,
# YOLO11n-cls on the vehicles, tiler, OSD and fakesink (--no-display). Checks the exit status, one count line per
# frame of each source with no more labelled objects than vehicles, vehicles on most frames and labels on most of
# those, and the totals; prints them and the frame rate. Skips (77) without a ROCm device, without rdsinfer, rdstracker
# or rdsosd in the build (the 4th, 5th and 6th arguments are 'infer', 'tracker' and 'osd' when it has them), without
# YOLO11n (tests/models/fetch-yolo11n.sh), YOLO11n-cls (tests/models/fetch-yolo11n-cls.sh) or the street clip
# (tests/models/fetch-tracker-clip.sh). The output is kept in <work dir>/rds-test2-run.log. The programs are cached
# next to the models, as when the sample runs on its own.
# bash for seq and date +%N (GNU coreutils). The clip's path goes into file:// URIs unescaped: source trees whose path
# has spaces or '%' are not supported.
set -eu
app=$1 src=$2 work=$3

# the street clip's frame count (as in pipeline_tracker)
CLIP_FRAMES=1356
SOURCES=4
clip=$src/.cache/media/crosswalk-720p.h264

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
if [ ! -f "$src/.cache/models/yolo11n-cls.onnx" ] || [ ! -f "$src/.cache/models/imagenet-labels.txt" ]; then
  echo 'YOLO11n-cls not found: run tests/models/fetch-yolo11n-cls.sh; skipped'
  exit 77
fi
if [ ! -f "$clip" ]; then
  echo 'street clip not found: run tests/models/fetch-tracker-clip.sh; skipped'
  exit 77
fi

fail() {
  echo "FAIL: $*"
  exit 1
}

log=$work/rds-test2-run.log
uris=
for _ in $(seq $SOURCES); do
  uris="$uris file://$clip"
done
t0=$(date +%s.%N)
status=0
# shellcheck disable=SC2086
"$app" --no-display $uris >"$log" 2>&1 || status=$?
t1=$(date +%s.%N)
grep -v '^source ' "$log" || true
[ "$status" -eq 0 ] || fail "rds-test2 exited with $status"

# one line per frame of each source, frame numbers 0 .. CLIP_FRAMES-1 in order
pattern='^source [0-9]+ frame [0-9]+: objects [0-9]+ \(vehicles: [0-9]+, labelled: [0-9]+\)$'
lines=$(grep -c '^source ' "$log" || true)
[ "$(grep -cE "$pattern" "$log" || true)" -eq "$lines" ] || fail 'count lines not in the expected format'
[ "$lines" -eq $((SOURCES * CLIP_FRAMES)) ] || fail "$lines count lines, expected $((SOURCES * CLIP_FRAMES))"
for s in $(seq 0 $((SOURCES - 1))); do
  grep "^source $s frame " "$log" | awk -v want=$CLIP_FRAMES -v s="$s" '
    { split($4, f, ":"); if (f[1] != NR - 1) { print "source " s ": frame " f[1] " at line " NR; bad = 1; exit } }
    END { if (!bad && NR != want) { print "source " s ": " NR " frames"; bad = 1 } exit bad }' ||
    fail "source $s: frames missing or out of order"
done

# only vehicles are classified; the clip is a busy intersection with cars in most frames, most of them big enough for
# the classifier (input-object-min-width/height 64)
grep '^source ' "$log" | tr -d '(),' | awk '
  { obj = $6; veh = $8; lab = $10
    if (veh > obj || lab > veh) { print "bad counts: " $0; exit 1 }
    if (veh > 0) with_veh++; if (lab > 0) with_lab++; objects += obj; vehicles += veh; labelled += lab }
  END { printf "%d frames: %d with vehicles, %d with labelled vehicles; objects %d, vehicles %d, labelled %d\n",
          NR, with_veh, with_lab, objects, vehicles, labelled
        if (with_veh * 2 < NR) { print "vehicles on less than half of the frames"; exit 1 }
        if (with_lab * 2 < with_veh) { print "labels on less than half of the frames with vehicles"; exit 1 } }' ||
  fail 'counts'
totals=$(grep -E '^frames [0-9]+, objects [0-9]+, vehicles [0-9]+, labelled [0-9]+$' "$log") ||
  fail 'no totals line'
[ "$(echo "$totals" | cut -d' ' -f2 | tr -d ,)" -eq "$lines" ] || fail 'the totals line counts other frames'
grep -qE '^label [^ ]+: [0-9]+$' "$log" || fail 'no label counts'
grep -m 3 '^source ' "$log"
awk -v n="$lines" -v t0="$t0" -v t1="$t1" 'BEGIN {
  printf "%d frames in %.1f s: %.1f fps (start-up and any compile included)\n", n, t1 - t0, n / (t1 - t0) }'
echo 'ok'
