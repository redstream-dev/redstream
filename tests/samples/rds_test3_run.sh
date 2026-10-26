#!/bin/bash
# Usage: rds_test3_run.sh <rds-test3> <source dir> <work dir> [infer] [tracker]
# rds-test3 end to end: four copies of the street clip, YOLO11n, the NvSORT tracker, tiler, OSD and fakesink
# (--no-display). Checks the exit status, one count line per frame of each source, and that most frames have objects;
# prints the frame rate. Skips (77) without a ROCm device, without rdsinfer or rdstracker in the build (the 4th and 5th
# arguments are 'infer' and 'tracker' when it has them), without YOLO11n (tests/models/fetch-yolo11n.sh) or without
# the street clip (tests/models/fetch-tracker-clip.sh). The output is kept in <work dir>/rds-test3-run.log.
# bash for seq and date +%N (GNU coreutils). The clip's path goes into file:// URIs unescaped: source trees whose path
# has spaces or '%' are not supported.
set -eu
app=$1 src=$2 work=$3

# the street clip's frame count (as in pipeline_tracker)
CLIP_FRAMES=1356
SOURCES=4
clip=$src/.cache/media/crosswalk-720p.h264

if [ "${4:-}" != infer ] || [ "${5:-}" != tracker ]; then
  echo 'built without rdsinfer or rdstracker: skipped'
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

fail() {
  echo "FAIL: $*"
  exit 1
}

log=$work/rds-test3-run.log
uris=
for _ in $(seq $SOURCES); do
  uris="$uris file://$clip"
done
t0=$(date +%s.%N)
status=0
# shellcheck disable=SC2086
"$app" --no-display --tracker-config "$src/samples/tracker/config_tracker_NvSORT.yml" $uris >"$log" 2>&1 || status=$?
t1=$(date +%s.%N)
grep -v '^source ' "$log" || true
[ "$status" -eq 0 ] || fail "rds-test3 exited with $status"

# one line per frame of each source, frame numbers 0 .. CLIP_FRAMES-1 in order
pattern='^source [0-9]+ frame [0-9]+: objects [0-9]+ \(person: [0-9]+, cars: [0-9]+\)$'
lines=$(grep -c '^source ' "$log" || true)
[ "$(grep -cE "$pattern" "$log" || true)" -eq "$lines" ] || fail 'count lines not in the expected format'
[ "$lines" -eq $((SOURCES * CLIP_FRAMES)) ] || fail "$lines count lines, expected $((SOURCES * CLIP_FRAMES))"
for s in $(seq 0 $((SOURCES - 1))); do
  grep "^source $s frame " "$log" | awk -v want=$CLIP_FRAMES -v s="$s" '
    { split($4, f, ":"); if (f[1] != NR - 1) { print "source " s ": frame " f[1] " at line " NR; bad = 1; exit } }
    END { if (!bad && NR != want) { print "source " s ": " NR " frames"; bad = 1 } exit bad }' ||
    fail "source $s: frames missing or out of order"
done

# objects (and people) on most frames: the clip is a busy crosswalk
with_objects=$(grep -cE 'objects [1-9]' "$log" || true)
with_people=$(grep -cE 'person: [1-9]' "$log" || true)
echo "$lines frames: $with_objects with objects, $with_people with people"
[ $((with_objects * 10)) -ge $((lines * 9)) ] || fail 'objects on less than 90% of the frames'
[ $((with_people * 10)) -ge $((lines * 9)) ] || fail 'people on less than 90% of the frames'
grep -m 3 '^source ' "$log"
awk -v n="$lines" -v t0="$t0" -v t1="$t1" 'BEGIN {
  printf "%d frames in %.1f s: %.1f fps (start-up and any compile included)\n", n, t1 - t0, n / (t1 - t0) }'
echo 'ok'
