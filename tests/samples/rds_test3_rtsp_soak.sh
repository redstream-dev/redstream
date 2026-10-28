#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Usage: rds_test3_rtsp_soak.sh <rds-test3> <rds-rtsp-serve> <source dir> <work dir> [infer] [osd] [asan]
#
# The RTSP soak, manual: it runs only with DURATION set (`DURATION=1800 meson test -C build-rel rds_test3_rtsp_soak`,
# with DURATION longer than the test's timeout also `-t N`), and skips otherwise, so a plain `meson test`, which runs
# every suite, doesn't take half an hour. Four rds-rtsp-serve instances (640x480 H.264, each with its fault relay) feed
# rds-test3 --no-display through the relays, with --rtsp-reconnect-interval RECONNECT_INTERVAL and
# --select-rtp-protocol 4, for DURATION seconds. A seeded random schedule hits one source every 1 to 3 minutes,
# after a minute of warm-up: a cut, or a freeze of 5 to 20 s then pass. Then SIGINT, and rds-test3 must stop with a
# clean EOS and status 0.
#
# Environment: DURATION (s; 1800 is the usual run), SEED (default: from the clock; printed, to replay a run),
# RECONNECT_INTERVAL (s, default 2), PGIE_CONFIG (default: scale2, the committed model with tensor output only, which
# keeps up with four live streams on a small GPU; give a config, e.g. the sample's YOLO11n one, to soak a detector).
#
# Prints the schedule, then a summary: reconnections and give-ups per source, frames per source per minute, the
# longest time each source went without a frame (counted in 10 s windows, so a lower bound), and rds-test3's RSS after
# the warm-up (2 min) and at the end. Fails when a source went without frames for longer than the longest freeze plus
# the interval plus 20 s, gave up, when the RSS grew by 5 % or more after the warm-up (not under ASan, the 7th
# argument 'asan': its quarantine and shadow memory grow on their own schedule), on a sanitizer report or when
# rds-test3 did not stop cleanly. Skips (77) without a ROCm device, rds-rtsp-serve (gst-rtsp-server), the VA encoder,
# or rdsinfer or rdsosd in the build (the 5th and 6th arguments are 'infer' and 'osd' when it has them). Everything is
# kept in <work dir>/rds-test3-rtsp-soak/.
set -eu
app=$1 serve=$2 src=$3 work=$4

if [ -z "${DURATION:-}" ]; then
  echo 'manual soak: set DURATION (seconds, e.g. 1800) to run it; skipped'
  exit 77
fi
SEED=${SEED:-$(date +%s)}
RECONNECT_INTERVAL=${RECONNECT_INTERVAL:-2}
SOURCES=4
WARMUP=60      # no fault before
RSS_AT=120     # the RSS baseline, once caches and pools are warm
WINDOW=10      # frame counting step, s
MAX_FREEZE=20

if [ "${5:-}" != infer ] || [ "${6:-}" != osd ]; then
  echo 'built without rdsinfer or rdsosd: skipped'
  exit 77
fi
if [ -z "$serve" ]; then
  echo 'built without gst-rtsp-server: skipped'
  exit 77
fi
if [ ! -e /dev/kfd ]; then
  echo 'no ROCm device (/dev/kfd): skipped'
  exit 77
fi

dir=$work/rds-test3-rtsp-soak
pids=()
app_pid=
cleanup() {
  [ -z "$app_pid" ] || kill -KILL "$app_pid" 2>/dev/null || true
  for p in ${pids[@]+"${pids[@]}"}; do
    kill "$p" 2>/dev/null || true
  done
}
trap cleanup EXIT
rm -rf "$dir"
mkdir -p "$dir"
failures=0
fail() {
  echo "FAIL: $*"
  failures=$((failures + 1))
}

# ---- the fault schedule ----
RANDOM=$SEED
echo "seed $SEED, duration $DURATION s, rtsp-reconnect-interval $RECONNECT_INTERVAL s"
declare -a faults
t=$WARMUP
while :; do
  t=$((t + 60 + RANDOM % 121))
  [ $t -lt $((DURATION - 30)) ] || break
  s=$((RANDOM % SOURCES))
  if [ $((RANDOM % 2)) -eq 0 ]; then
    f="$t:cut"
  else
    d=$((5 + RANDOM % (MAX_FREEZE - 4)))
    f="$t:freeze,$((t + d)):pass"
  fi
  faults[$s]="${faults[$s]:+${faults[$s]},}$f"
  echo "  at $t s: source $s ${f#*:}"
done

# ---- the servers ----
uris=
for s in $(seq 0 $((SOURCES - 1))); do
  args=(--port 0 --proxy-port 0 --size 640x480)
  [ -z "${faults[$s]:-}" ] || args+=(--faults "${faults[$s]}")
  "$serve" "${args[@]}" >"$dir/serve$s.out" 2>"$dir/serve$s.err" &
  pids+=($!)
  i=0
  until grep -q '^relay ' "$dir/serve$s.out"; do
    if ! kill -0 "${pids[$s]}" 2>/dev/null; then
      if grep -q 'no vah264enc' "$dir/serve$s.err"; then
        echo 'no VA H.264 encoder: skipped'
        exit 77
      fi
      echo "FAIL: server $s did not start: $(cat "$dir/serve$s.err")"
      exit 1
    fi
    i=$((i + 1))
    [ $i -lt 100 ] || { echo "FAIL: server $s: no URLs after 10 s"; exit 1; }
    sleep 0.1
  done
  uris="$uris $(sed -n 's/^relay //p' "$dir/serve$s.out")"
done

if [ -z "${PGIE_CONFIG:-}" ]; then
  cp -f "$src/tests/models/scale2.onnx" "$work/scale2.onnx"
  PGIE_CONFIG=$dir/pgie.txt
  cat >"$PGIE_CONFIG" <<EOF
[property]
onnx-file=$work/scale2.onnx
infer-dims=3;640;640
batch-size=$SOURCES
network-type=100
net-scale-factor=0.00392156862745098
maintain-aspect-ratio=1
symmetric-padding=1
EOF
fi

# ---- the run ----
log=$dir/app.log
# shellcheck disable=SC2086 # the URIs split on purpose
"$app" --no-display --pgie-config "$PGIE_CONFIG" --rtsp-reconnect-interval "$RECONNECT_INTERVAL" \
  --select-rtp-protocol 4 $uris >"$log" 2>&1 &
app_pid=$!
start=$(date +%s)
rss() {
  awk '/^VmRSS:/ { print $2 }' "/proc/$app_pid/status" 2>/dev/null || echo 0
}
rss_warm=0
: >"$dir/windows"
while :; do
  sleep $WINDOW
  now=$(($(date +%s) - start))
  kill -0 $app_pid 2>/dev/null || { fail "rds-test3 ended after $now s"; break; }
  counts=
  for s in $(seq 0 $((SOURCES - 1))); do
    counts="$counts $(grep -c "^source $s frame " "$log" || true)"
  done
  echo "$now$counts $(rss)" >>"$dir/windows"
  [ $rss_warm -ne 0 ] || [ $now -lt $RSS_AT ] || rss_warm=$(rss)
  [ $now -lt "$DURATION" ] || break
done
rss_end=$(rss)

status=0
if kill -0 $app_pid 2>/dev/null; then
  kill -INT $app_pid
  for _ in $(seq 300); do
    kill -0 $app_pid 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 $app_pid 2>/dev/null; then
    fail 'rds-test3 still running 30 s after SIGINT'
    kill -KILL $app_pid
  fi
  wait $app_pid || status=$?
fi
app_pid=
[ "$status" -eq 0 ] || fail "rds-test3 exited with $status"
grep -qx 'End of stream' "$log" || fail 'no clean end of stream'
if grep -q 'No end of stream within' "$log"; then
  fail 'the EOS timed out'
fi
if grep -qE 'AddressSanitizer|LeakSanitizer|runtime error:' "$log"; then
  fail 'sanitizer report'
  grep -E -A20 'AddressSanitizer|LeakSanitizer|runtime error:' "$log" | head -60
fi
if grep -q '^Error from' "$log"; then
  fail 'ERROR on the bus'
  grep -A2 '^Error from' "$log" | head -20
fi

# ---- the summary ----
echo
echo "== summary (seed $SEED)"
for s in $(seq 0 $((SOURCES - 1))); do
  a=$(grep -c "^source $s: reconnecting" "$log" || true)
  b=$(grep -cx "source $s: reconnected" "$log" || true)
  g=$(grep -cx "source $s: gave up reconnecting" "$log" || true)
  echo "source $s: $a attempts, $b reconnections, $g give-ups; faults: ${faults[$s]:-none}"
  [ "$g" -eq 0 ] || fail "source $s gave up"
done
echo
echo "frames per minute (minute: source 0..$((SOURCES - 1)); each window's rate, averaged over the minute):"
awk -v n=$SOURCES '
  { m = int(($1 - 1) / 60); dt = $1 - t; t = $1
    for (s = 0; s < n; s++) { rate[m, s] += ($(s + 2) - prev[s]) * 60 / dt; prev[s] = $(s + 2) }
    windows[m]++; if (m > maxm) maxm = m }
  END { for (m = 0; m <= maxm; m++) { line = sprintf("%4d:", m)
          for (s = 0; s < n; s++) line = line sprintf(" %6.0f", rate[m, s] / windows[m])
          print line } }' "$dir/windows"
allowed=$((MAX_FREEZE + RECONNECT_INTERVAL + 20))
echo
echo "longest time without a frame (allowed $allowed s after the first frame):"
for s in $(seq 0 $((SOURCES - 1))); do
  longest=$(awk -v col=$((s + 2)) -v w=$WINDOW '
    { if ($col > 0 && seen && $col == prev) { run += w } else { run = 0 }
      if ($col > 0) seen = 1; if (run > max) max = run; prev = $col }
    END { print max + 0 }' "$dir/windows")
  echo "source $s: about $longest s"
  [ "$longest" -le $allowed ] || fail "source $s went $longest s without a frame"
done
echo
echo "RSS: $rss_warm kB after the warm-up, $rss_end kB at the end"
if [ "${7:-}" = asan ]; then
  echo 'ASan build: the RSS is not checked'
elif [ "$rss_warm" -gt 0 ]; then
  growth=$(awk -v a=$rss_warm -v b=$rss_end 'BEGIN { printf "%.1f", (b - a) * 100 / a }')
  echo "RSS growth: $growth %"
  awk -v g="$growth" 'BEGIN { exit !(g < 5) }' || fail "RSS grew by $growth %"
else
  fail 'no RSS after the warm-up (a run shorter than 2 min?)'
fi
[ $failures -eq 0 ] || { echo "$failures failure(s)"; exit 1; }
echo 'ok'
