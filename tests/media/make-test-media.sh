#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Generate small H.264 test clips with the VA encoder (no GPL encoders needed).
# Usage: tests/media/make-test-media.sh [outdir]   (default: .cache/media)
set -eu
out="${1:-.cache/media}"
mkdir -p "$out"

gen() { # name width height frames pattern
  gst-launch-1.0 -q videotestsrc pattern="$5" num-buffers="$4" \
    ! "video/x-raw,width=$2,height=$3,framerate=30/1,format=NV12" \
    ! vah264enc ! h264parse ! "video/x-h264,stream-format=byte-stream,alignment=au" \
    ! filesink location="$out/$1.h264"
}

gen ball-720p      1280 720 300 ball
gen smpte-1366x768 1366 768 120 smpte   # width not a multiple of 64: exercises pitch padding
gen ball-1080p     1920 1080 300 ball   # coded height 1088: exercises display cropping
ls -l "$out"
