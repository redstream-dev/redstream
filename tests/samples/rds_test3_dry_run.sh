#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Usage: rds_test3_dry_run.sh <path-to-rds-test3> [<path-to-gst-inspect-1.0> [infer] [tracker] [osd]]
# The third, fourth and fifth arguments are 'infer', 'tracker' and 'osd' when the build has rdsinfer, rdstracker and
# rdsosd: then they are inspected too.
set -eu
out=$("$1" --dry-run file:///dev/null file:///dev/null)
echo "$out"
echo "$out" | grep -qx 'sources: 2'
echo "$out" | grep -qx 'nvurisrcbin -> rdsurisrcbin'
echo "$out" | grep -qx 'nvstreammux -> rdsstreammux'
echo "$out" | grep -qx 'nvinfer -> rdsinfer'
echo "$out" | grep -qx 'nvmultistreamtiler -> rdsmultistreamtiler'
echo "$out" | grep -qx 'nvvideoconvert -> rdsvideoconvert'
echo "$out" | grep -qx 'nvdsosd -> rdsosd'
echo "$out" | grep -qx 'nveglglessink -> rdsvideosink'
# the default PGIE config is the sample's own, wherever the binary runs from
pgie=$(echo "$out" | sed -n 's/^pgie config: //p')
test -f "$pgie"
if echo "$out" | grep -q nvtracker; then
  exit 1
fi

# --tracker-config adds the tracker right after the primary detector
out=$("$1" --dry-run --tracker-config tracker.yml file:///dev/null)
echo "$out"
echo "$out" | grep -A1 -x 'nvinfer -> rdsinfer' | grep -qx 'nvtracker -> rdstracker'
# --analytics-config adds the analytics right after the tracker (after the primary detector without one); without it
# there is none
if echo "$out" | grep -q nvdsanalytics; then
  exit 1
fi
out=$("$1" --dry-run --tracker-config tracker.yml --analytics-config analytics.txt file:///dev/null)
echo "$out"
echo "$out" | grep -A1 -x 'nvtracker -> rdstracker' | grep -qx 'nvdsanalytics -> rdsanalytics'
echo "$out" | grep -A1 -x 'nvdsanalytics -> rdsanalytics' | grep -qx 'nvmultistreamtiler -> rdsmultistreamtiler'
out=$("$1" --dry-run --analytics-config analytics.txt file:///dev/null)
echo "$out" | grep -A1 -x 'nvinfer -> rdsinfer' | grep -qx 'nvdsanalytics -> rdsanalytics'
# --no-display swaps the display sink for fakesink, nothing else
out=$("$1" --dry-run --no-display file:///dev/null)
echo "$out"
echo "$out" | grep -A1 -x 'nvdsosd -> rdsosd' | grep -qx 'fakesink -> fakesink'
if echo "$out" | grep -q nveglglessink; then
  exit 1
fi
# --output replaces the display sink with the encoding chain: H.264 by default, the muxer from the extension
out=$("$1" --dry-run --output out.mp4 file:///dev/null)
echo "$out"
chain='nvdsosd -> rdsosd nvvideoconvert -> rdsvideoconvert nvv4l2h264enc -> rdsh264enc h264parse -> h264parse '
chain="${chain}qtmux -> qtmux filesink -> filesink "
echo "$out" | grep -A6 -x 'nvdsosd -> rdsosd' | tr '\n' ' ' | grep -qxF "$chain"
echo "$out" | grep -qx 'output: out.mp4 (h264, qtmux)'
if echo "$out" | grep -q -e nveglglessink -e fakesink; then
  exit 1
fi
out=$("$1" --dry-run --output out.mkv --codec h265 file:///dev/null)
echo "$out"
chain='nvdsosd -> rdsosd nvvideoconvert -> rdsvideoconvert nvv4l2h265enc -> rdsh265enc h265parse -> h265parse '
chain="${chain}matroskamux -> matroskamux filesink -> filesink "
echo "$out" | grep -A6 -x 'nvdsosd -> rdsosd' | tr '\n' ' ' | grep -qxF "$chain"
# the extension in any case; --codec h264 explicitly
"$1" --dry-run --output OUT.MKV --codec h264 file:///dev/null | grep -qx 'nvv4l2h264enc -> rdsh264enc'
# with --no-display too, --output wins: no fakesink
out=$("$1" --dry-run --no-display --output out.mp4 file:///dev/null)
echo "$out" | grep -qx 'filesink -> filesink'
if echo "$out" | grep -q fakesink; then
  exit 1
fi
# refused: another extension, none, an empty name, another codec, --codec without --output
for bad in '--output out.avi' '--output out' '--output out.mp4 --codec vp9' '--output out.mp4 --codec H264' \
  '--codec h265'; do
  # shellcheck disable=SC2086
  if "$1" --dry-run $bad file:///dev/null >/dev/null 2>&1; then
    echo "accepted: $bad"
    exit 1
  fi
done
if "$1" --dry-run --output '' file:///dev/null >/dev/null 2>&1; then
  exit 1
fi
"$1" --dry-run --output out.avi file:///dev/null 2>&1 | grep -q 'out.avi.*\.mp4.*\.mkv'
"$1" --dry-run --output out.mp4 --codec vp9 file:///dev/null 2>&1 | grep -q 'vp9.*h264.*h265'
"$1" --help | grep -q -- '--output'
"$1" --help | grep -q -- '--codec'

# the RTSP options are accepted
"$1" --dry-run --rtsp-reconnect-interval 2 --rtsp-reconnect-attempts 3 --select-rtp-protocol 4 \
  rtsp://127.0.0.1:1/test >/dev/null
"$1" --help | grep -q -- '--rtsp-reconnect-interval'
if "$1" --dry-run --select-rtp-protocol 3 rtsp://127.0.0.1:1/test >/dev/null 2>&1; then
  exit 1
fi
# an empty file name is refused
if "$1" --dry-run --tracker-config '' file:///dev/null >/dev/null 2>&1; then
  exit 1
fi
if "$1" --dry-run --analytics-config '' file:///dev/null >/dev/null 2>&1; then
  exit 1
fi

# nvurisrcbin, nvv4l2decoder, nvvideoconvert, nvmultistreamtiler, nveglglessink, nvdsanalytics, the two encoders and
# (when built) nvinfer, nvtracker and nvdsosd map to real elements of the build tree (GST_PLUGIN_PATH_1_0 comes from the
# test environment); --output's stock parsers, muxers and filesink are installed.
if [ -n "${2:-}" ]; then
  # one element per call: given several names, gst-inspect-1.0 only looks at the last one
  elements='rdsurisrcbin rdsvideodec rdsvideoconvert rdsmultistreamtiler rdsvideosink rdsanalytics'
  elements="$elements rdsh264enc rdsh265enc h264parse h265parse qtmux matroskamux filesink"
  if [ "${3:-}" = infer ]; then
    elements="$elements rdsinfer"
  fi
  if [ "${4:-}" = tracker ]; then
    elements="$elements rdstracker"
  fi
  if [ "${5:-}" = osd ]; then
    elements="$elements rdsosd"
  fi
  for e in $elements; do
    "$2" "$e" >/dev/null || exit 1
    echo "gst-inspect-1.0 $e: ok"
  done
else
  echo 'no gst-inspect-1.0 given (not found, or a sanitized build): element check skipped'
fi
