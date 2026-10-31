#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Usage: rds_test2_dry_run.sh <path-to-rds-test2> [<path-to-gst-inspect-1.0> [infer] [tracker] [osd]]
# The third, fourth and fifth arguments are 'infer', 'tracker' and 'osd' when the build has rdsinfer, rdstracker and
# rdsosd: then they are inspected too.
set -eu
out=$("$1" --dry-run file:///dev/null file:///dev/null)
echo "$out"
echo "$out" | grep -qx 'sources: 2'
echo "$out" | grep -qx 'nvurisrcbin -> rdsurisrcbin'
# the pipeline in order: mux, the detector, the tracker, the classifier, tiler, converter, OSD, display
mapping=$(echo "$out" | grep ' -> ' | grep -v '^nvurisrcbin ')
expected='nvstreammux -> rdsstreammux
nvinfer -> rdsinfer
nvtracker -> rdstracker
nvinfer -> rdsinfer
nvmultistreamtiler -> rdsmultistreamtiler
nvvideoconvert -> rdsvideoconvert
nvdsosd -> rdsosd
nveglglessink -> rdsvideosink'
[ "$mapping" = "$expected" ]
# the default configs are the sample's own (and the sample tracker config), wherever the binary runs from
for key in 'pgie config' 'sgie config' 'tracker config'; do
  f=$(echo "$out" | sed -n "s/^$key: //p")
  test -f "$f"
done
echo "$out" | grep -q '^pgie config: .*/rds_test2_pgie_config.yml$'
echo "$out" | grep -q '^sgie config: .*/rds_test2_sgie_config.yml$'
echo "$out" | grep -q '^tracker config: .*/config_tracker_NvSORT.yml$'
# the options replace them
out=$("$1" --dry-run --pgie-config p.txt --sgie-config s.txt --tracker-config t.yml file:///dev/null)
echo "$out" | grep -qx 'pgie config: p.txt'
echo "$out" | grep -qx 'sgie config: s.txt'
echo "$out" | grep -qx 'tracker config: t.yml'
# --no-display swaps the display sink for fakesink, nothing else
out=$("$1" --dry-run --no-display file:///dev/null)
echo "$out"
echo "$out" | grep -A1 -x 'nvdsosd -> rdsosd' | grep -qx 'fakesink -> fakesink'
if echo "$out" | grep -q nveglglessink; then
  exit 1
fi
# no URI, or an empty file name, is refused
if "$1" --dry-run >/dev/null 2>&1; then
  exit 1
fi
if "$1" --dry-run --sgie-config '' file:///dev/null >/dev/null 2>&1; then
  exit 1
fi

# the mapped elements are real elements of the build tree (GST_PLUGIN_PATH_1_0 comes from the test environment)
if [ -n "${2:-}" ]; then
  # one element per call: given several names, gst-inspect-1.0 only looks at the last one
  elements='rdsurisrcbin rdsstreammux rdsmultistreamtiler rdsvideoconvert rdsvideosink'
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
