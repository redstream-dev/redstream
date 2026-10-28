#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Usage: rds_rtsp_serve.sh <rds-rtsp-serve> <gst-launch-1.0> <work dir>
# rds-rtsp-serve: --help, option errors, then a server with the relay on free ports, 30 frames through the relay with
# gst-launch-1.0, a client that plays until a scheduled cut ends it, a clean exit on SIGTERM, and --file with an MP4
# (audio first, when opusenc is there) and a byte stream, under names with '%', '"' and a space. An empty
# <rds-rtsp-serve> (built without gst-rtsp-server), no gst-launch-1.0 or a missing VA encoder skips.
# <work dir>/rds-rtsp-serve is removed on exit.
set -eu
serve=$1 launch=$2 work=$3

if [ -z "$serve" ]; then
  echo 'built without gst-rtsp-server: skipped'
  exit 77
fi
if [ -z "$launch" ]; then
  echo 'no gst-launch-1.0: skipped'
  exit 77
fi

dir=$work/rds-rtsp-serve
pid=
cleanup() {
  [ -z "$pid" ] || kill "$pid" 2>/dev/null || true
  rm -rf "$dir"
}
trap cleanup EXIT
rm -rf "$dir"
mkdir -p "$dir"

fail() {
  echo "FAIL: $*"
  [ ! -f "$dir/err" ] || cat "$dir/err"
  exit 1
}

"$serve" --help | grep -q -- '--faults' || fail '--help'
for bad in '--faults 1:cut' '--faults 1:melt --proxy-port 0' '--size 12' '--port 70000' '--codec vp9' 'extra'; do
  # shellcheck disable=SC2086 # split on purpose
  if "$serve" $bad >/dev/null 2>"$dir/err"; then
    fail "accepted $bad"
  fi
done

# Starts "$serve" "$@" on the side and waits for its URLs; exits 77 without the VA encoder.
start() {
  "$serve" "$@" >"$dir/out" 2>"$dir/err" &
  pid=$!
  i=0
  until grep -q '^direct ' "$dir/out"; do
    if ! kill -0 "$pid" 2>/dev/null; then
      pid=
      if grep -q 'no vah264enc' "$dir/err"; then
        echo 'no VA H.264 encoder: skipped'
        exit 77
      fi
      fail 'the server did not start'
    fi
    i=$((i + 1))
    [ $i -lt 100 ] || fail 'no URLs after 10 s'
    sleep 0.1
  done
}

stop() {
  kill -TERM "$pid"
  status=0
  wait "$pid" || status=$?
  pid=
  [ $status -eq 0 ] || fail "exit status $status after SIGTERM"
}

now_ms() {
  echo $(($(date +%s%N) / 1000000))
}

# a free port each, a cut 12 s after the start (well after the second client below has started)
start --port 0 --proxy-port 0 --size 320x240 --faults 12:cut
grep -q '^direct rtsp://127.0.0.1:[0-9]*/test$' "$dir/out" || fail "direct URL: $(cat "$dir/out")"
relay=$(sed -n 's/^relay //p' "$dir/out")
[ -n "$relay" ] || fail "no relay URL: $(cat "$dir/out")"

"$launch" -q rtspsrc location="$relay" protocols=tcp ! rtph264depay ! h264parse ! fakesink num-buffers=30 \
  || fail 'no 30 frames through the relay'
# this client plays (fakesink logs each buffer) until the cut, then rtspsrc ends it with EOS or an error
grep -q '12:cut' "$dir/err" && fail 'the cut came before the second client started'
t0=$(now_ms)
status=0
timeout 30 "$launch" -v rtspsrc location="$relay" protocols=tcp ! fakesink silent=false >"$dir/client" 2>&1 \
  || status=$?
elapsed=$(($(now_ms) - t0))
[ $status -ne 124 ] || fail 'the cut did not end the client'
[ "$(grep -c 'last-message = chain' "$dir/client")" -ge 30 ] || fail 'the second client got no 30 buffers'
grep -q -e 'Got EOS' -e 'ERROR' "$dir/client" || fail 'the second client ended without EOS or an error'
[ $elapsed -ge 2000 ] || fail "the second client ended after $elapsed ms"
grep -q '12:cut' "$dir/err" || fail 'the cut was not logged'
stop

# --file: an MP4 whose first track is audio (when opusenc is there), and a byte stream, under names that need
# quoting
mp4="$dir/clip 100%.mp4"
h264="$dir/clip \"%d\".h264"
video='videotestsrc num-buffers=90 ! video/x-raw,format=NV12,width=320,height=240,framerate=30/1 ! vah264enc
  ! h264parse'
if command -v gst-inspect-1.0 >/dev/null && gst-inspect-1.0 opusenc >/dev/null 2>&1; then
  # shellcheck disable=SC2086 # split on purpose
  "$launch" -q mp4mux name=m ! filesink location="$mp4" audiotestsrc num-buffers=150 ! opusenc ! m.audio_0 \
    $video ! m.video_0 || fail 'encoding the MP4 with audio'
else
  # shellcheck disable=SC2086
  "$launch" -q $video ! mp4mux ! filesink location="$mp4" || fail 'encoding the MP4'
fi
# shellcheck disable=SC2086
"$launch" -q $video ! video/x-h264,stream-format=byte-stream ! filesink location="$h264" \
  || fail 'encoding the byte stream'
tmps() {
  ls -d "${TMPDIR:-/tmp}"/rds-rtsp-serve-* 2>/dev/null | wc -l
}
tmps_before=$(tmps)
for f in "$mp4" "$h264"; do
  start --port 0 --file "$f"
  direct=$(sed -n 's/^direct //p' "$dir/out")
  # 120 frames: past the end of the 90-frame clip, so it loops
  "$launch" -q rtspsrc location="$direct" protocols=tcp ! rtph264depay ! h264parse ! fakesink num-buffers=120 \
    || fail "no 120 frames from --file $f"
  stop
done
[ "$(tmps)" -eq "$tmps_before" ] || fail 'a temporary file was left behind'
echo ok
