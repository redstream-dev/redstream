#!/bin/sh
# Usage: rds_test3_dry_run.sh <path-to-rds-test3> [<path-to-gst-inspect-1.0>]
set -eu
out=$("$1" --dry-run file:///dev/null file:///dev/null)
echo "$out"
echo "$out" | grep -qx 'sources: 2'
echo "$out" | grep -qx 'nvstreammux -> rdsstreammux'
echo "$out" | grep -qx 'nvinfer -> rdsinfer'
echo "$out" | grep -qx 'nvmultistreamtiler -> rdsmultistreamtiler'
echo "$out" | grep -qx 'nvvideoconvert -> rdsvideoconvert'
echo "$out" | grep -qx 'nvdsosd -> rdsosd'
echo "$out" | grep -qx 'nveglglessink -> rdsvideosink'

# nvv4l2decoder and nvvideoconvert now map to real elements of the build tree (GST_PLUGIN_PATH_1_0 comes from the
# test environment).
if [ -n "${2:-}" ]; then
  "$2" rdsvideodec rdsvideoconvert >/dev/null
  echo 'gst-inspect-1.0 rdsvideodec rdsvideoconvert: ok'
else
  echo 'no gst-inspect-1.0 given (not found, or a sanitized build): element check skipped'
fi
