#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Usage: rds_test3_analytics.sh <rds-test3> <source dir> <work dir> [infer] [tracker] [osd]
# rds-test3 --analytics-config end to end: two copies of the street clip, YOLO11n, the NvSORT tracker, rdsanalytics
# with the sample's analytics config, tiler, OSD and fakesink (--no-display). Checks the exit status, one count line
# per frame of each source with the ROI and line-crossing counts, people in the ROI on some frames, at least one
# crossing, a cumulative count that never decreases, and the same lines for both sources (the config has the same
# rules on each); prints the totals and the frame rate. Skips (77) as rds_test3_run.sh does. The output is kept in
# <work dir>/rds-test3-analytics.log. The clip's path goes into file:// URIs unescaped: source trees whose path has
# spaces or '%' are not supported.
set -eu
app=$1 src=$2 work=$3

# the street clip's frame count (as in pipeline_tracker)
CLIP_FRAMES=1356
SOURCES=2
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
if [ ! -f "$clip" ]; then
  echo 'street clip not found: run tests/models/fetch-tracker-clip.sh; skipped'
  exit 77
fi

fail() {
  echo "FAIL: $*"
  exit 1
}

log=$work/rds-test3-analytics.log
uris=
for _ in $(seq $SOURCES); do
  uris="$uris file://$clip"
done
t0=$(date +%s.%N)
status=0
# shellcheck disable=SC2086
"$app" --no-display --tracker-config "$src/samples/tracker/config_tracker_NvSORT.yml" \
  --analytics-config "$src/samples/rds-test3/rds_test3_analytics_config.txt" $uris >"$log" 2>&1 || status=$?
t1=$(date +%s.%N)
grep -v '^source ' "$log" || true
[ "$status" -eq 0 ] || fail "rds-test3 exited with $status"

pattern='^source [0-9]+ frame [0-9]+: objects [0-9]+ \(person: [0-9]+, cars: [0-9]+\) '
pattern="${pattern}roi Crosswalk=[0-9]+ lc Eastbound=[0-9]+\$"
lines=$(grep -c '^source ' "$log" || true)
[ "$(grep -cE "$pattern" "$log" || true)" -eq "$lines" ] || fail 'count lines not in the expected format'
[ "$lines" -eq $((SOURCES * CLIP_FRAMES)) ] || fail "$lines count lines, expected $((SOURCES * CLIP_FRAMES))"
for s in $(seq 0 $((SOURCES - 1))); do
  # frames in order, the cumulative count never decreasing; prints "<frames with people in the ROI> <crossings>".
  # The awk fields ($4 the frame number, $12 the ROI count, $14 the cumulative crossings) depend on the line format
  # checked just above: change both together.
  totals=$(grep "^source $s frame " "$log" | awk -v want=$CLIP_FRAMES -v s="$s" '
    { split($4, f, ":"); split($12, r, "="); split($14, c, "=")
      if (f[1] != NR - 1) { print "source " s ": frame " f[1] " at line " NR > "/dev/stderr"; bad = 1; exit }
      if (c[2] + 0 < last) { print "source " s ": count down at frame " f[1] > "/dev/stderr"; bad = 1; exit }
      last = c[2] + 0; roi += r[2] > 0 }
    END { if (!bad && NR != want) { print "source " s ": " NR " frames" > "/dev/stderr"; bad = 1 }
      if (!bad) print roi, last; exit bad }') || fail "source $s: frames missing, out of order or counting down"
  echo "source $s: $CLIP_FRAMES frames, people in Crosswalk on ${totals% *}, ${totals#* } Eastbound crossings"
  [ "${totals% *}" -gt 0 ] || fail "source $s: nobody in the ROI"
  [ "${totals#* }" -ge 1 ] || fail "source $s: no crossing"
done
# the same rules on the same clip: the same lines
cmp -s <(grep '^source 0 ' "$log" | cut -d' ' -f3-) <(grep '^source 1 ' "$log" | cut -d' ' -f3-) ||
  fail 'sources 0 and 1 differ'
grep '^source 0 ' "$log" | tail -1
awk -v n="$lines" -v t0="$t0" -v t1="$t1" 'BEGIN {
  printf "%d frames in %.1f s: %.1f fps (start-up and any compile included)\n", n, t1 - t0, n / (t1 - t0) }'
echo 'ok'
