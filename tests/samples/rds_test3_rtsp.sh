#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Usage: rds_test3_rtsp.sh <rds-test3> <rds-rtsp-serve> <source dir> <work dir> [infer] [osd]
# rds-test3 on RTSP: one rds-rtsp-serve (320x240) with its relay; source 0 through the relay, source 1 direct, source
# 2 a URL nothing listens on. --rtsp-reconnect-interval 1, --select-rtp-protocol 4, --no-display, and the committed
# scale2 model as the detector (tensor output only, compiled afresh in the work directory each run: seconds, so no
# YOLO11n is needed). Once source 0's first frame is out, SIGUSR1 makes the server cut the relay; once source 0 is
# back, SIGINT: rds-test3 must stop with a clean EOS (source 2 never had a stream, so the app ends it at its muxer pad),
# within 15 s and with status 0. Checks frames from sources 0 and 1, source 0's reconnection and source 2's attempts.
# Skips (77) without a ROCm device, rds-rtsp-serve (gst-rtsp-server), the VA encoder, or rdsinfer or rdsosd in the
# build (the 5th and 6th arguments are 'infer' and 'osd' when it has them).
# The output is kept in <work dir>/rds-test3-rtsp/.
set -eu
app=$1 serve=$2 src=$3 work=$4

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

dir=$work/rds-test3-rtsp
pid= app_pid=
cleanup() {
  [ -z "$app_pid" ] || kill -KILL "$app_pid" 2>/dev/null || true
  [ -z "$pid" ] || kill "$pid" 2>/dev/null || true
}
trap cleanup EXIT
rm -rf "$dir"
mkdir -p "$dir"

fail() {
  echo "FAIL: $*"
  cat "$dir/app.log" 2>/dev/null | grep -v '^source [0-9]* frame' || true
  exit 1
}

"$serve" --port 0 --proxy-port 0 --size 320x240 >"$dir/serve.out" 2>"$dir/serve.err" &
pid=$!
i=0
until grep -q '^relay ' "$dir/serve.out"; do
  if ! kill -0 "$pid" 2>/dev/null; then
    pid=
    if grep -q 'no vah264enc' "$dir/serve.err"; then
      echo 'no VA H.264 encoder: skipped'
      exit 77
    fi
    fail "the server did not start: $(cat "$dir/serve.err")"
  fi
  i=$((i + 1))
  [ $i -lt 100 ] || fail 'no URLs after 10 s'
  sleep 0.1
done
relay=$(sed -n 's/^relay //p' "$dir/serve.out")
direct=$(sed -n 's/^direct //p' "$dir/serve.out")

# scale2 at 640x640, tensor output only (as pipeline_infer's soak without YOLO11n); a fresh copy, so its program is
# compiled at start, as on a cold cache
cp -f "$src/tests/models/scale2.onnx" "$dir/scale2.onnx"
cat >"$dir/pgie.txt" <<EOF
[property]
onnx-file=$dir/scale2.onnx
infer-dims=3;640;640
batch-size=3
network-type=100
net-scale-factor=0.00392156862745098
maintain-aspect-ratio=1
symmetric-padding=1
EOF

"$app" --no-display --pgie-config "$dir/pgie.txt" --rtsp-reconnect-interval 1 --select-rtp-protocol 4 \
  "$relay" "$direct" rtsp://127.0.0.1:1/never >"$dir/app.log" 2>&1 &
app_pid=$!
# Waits up to $2 s for a line of the app's log matching $1.
wait_log() {
  for _ in $(seq $(($2 * 10))); do
    grep -q "$1" "$dir/app.log" && return 0
    kill -0 $app_pid 2>/dev/null || fail "rds-test3 ended early"
    sleep 0.1
  done
  fail "no '$1' within $2 s"
}
wait_log '^source 0 frame ' 120
kill -USR1 $pid
wait_log '^source 0: reconnected$' 30
# two seconds of frames after the reconnection
sleep 2
kill -INT $app_pid
status=0
for _ in $(seq 150); do
  kill -0 $app_pid 2>/dev/null || break
  sleep 0.1
done
if kill -0 $app_pid 2>/dev/null; then
  kill -KILL $app_pid
  fail 'rds-test3 still running 15 s after SIGINT'
fi
wait $app_pid || status=$?
app_pid=
grep -v '^source [0-9]* frame' "$dir/app.log" | head -40
[ "$status" -eq 0 ] || fail "rds-test3 exited with $status"
grep -qx 'End of stream' "$dir/app.log" || fail 'no clean end of stream'
if grep -q 'No end of stream within' "$dir/app.log"; then
  fail 'the EOS timed out'
fi
for s in 0 1; do
  n=$(grep -c "^source $s frame " "$dir/app.log" || true)
  echo "source $s: $n frames"
  [ "$n" -ge 30 ] || fail "source $s: $n frames"
done
[ "$(grep -c '^source 2 frame ' "$dir/app.log" || true)" -eq 0 ] || fail 'frames from source 2'
grep -q '^source 0: reconnecting, attempt 1 ' "$dir/app.log" || fail 'source 0 did not reconnect after the cut'
grep -qx 'source 0: reconnected' "$dir/app.log" || fail 'source 0 did not come back'
grep -q '^source 2: reconnecting' "$dir/app.log" || fail 'no attempts for source 2'
echo 'ok'
