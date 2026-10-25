#!/bin/sh
# Usage: rds_test3_dry_run.sh <path-to-rds-test3> [<path-to-gst-inspect-1.0> [infer]]
# The third argument is 'infer' when the build has rdsinfer: then it is inspected too.
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

# nvv4l2decoder, nvvideoconvert and (when built) nvinfer now map to real elements of the build tree
# (GST_PLUGIN_PATH_1_0 comes from the test environment).
if [ -n "${2:-}" ]; then
  # one element per call: given several names, gst-inspect-1.0 only looks at the last one
  elements='rdsvideodec rdsvideoconvert'
  if [ "${3:-}" = infer ]; then
    elements="$elements rdsinfer"
  fi
  for e in $elements; do
    "$2" "$e" >/dev/null || exit 1
    echo "gst-inspect-1.0 $e: ok"
  done
else
  echo 'no gst-inspect-1.0 given (not found, or a sanitized build): element check skipped'
fi
