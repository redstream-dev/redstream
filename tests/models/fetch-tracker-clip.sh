#!/bin/sh
# Downloads the public-domain (CC0) street clip for the rdstracker tests from Wikimedia Commons and transcodes it to
# .cache/media/crosswalk-720p.h264: 1280x720 H.264 Annex-B, as rdsvideodec decodes it (see README.md). Runs on the
# host and needs Docker, network access and the GPU (the encoder is vah264enc in redstream:dev).
#
# The download is checked against a pinned sha256 before anything decodes it. The clip is VP8, which the dev image
# can't decode: a throwaway python:3.12-slim container decodes it with PyAV (pinned with its hash in
# tracker-clip-requirements.txt) and pipes raw frames into redstream:dev, which scales and encodes them; neither
# container has network access (only the pip install step has). The venv, pip cache and download stay in
# .cache/tracker-clip, so re-runs are fast. The output is redone whenever its recipe changes: a <name>.recipe file next
# to it holds the source hash, the decoder and its requirements' hashes, both image IDs and the encode pipeline.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out="$root/.cache/media"
work="$root/.cache/tracker-clip"
name=crosswalk-720p.h264
url=https://upload.wikimedia.org/wikipedia/commons/d/d2/DiagonalCrosswalkYongeDundas.webm
src_sha256=a497af7a6e905ae265fdc2dffd607eaa64a143ebd935137039a4e115bde0e01e
src_w=1920
src_h=1080
# The encoder output depends on the GPU and its driver: a different hash is only a warning.
h264_sha256=ec96845ec40ad844f80c25909477e65025c7f596acb3a7969cefccde754cca9a
image=python:3.12-slim@sha256:a0f57064cbdcd6285fa45f51237ab0b35ea8ebaed06a1f5973e89e19dfb86704
dev_image=${REDSTREAM_DEV_IMAGE:-redstream:dev}

check_h264 () {
  got=$(sha256sum "$out/$name" | cut -d' ' -f1)
  if [ "$got" != "$h264_sha256" ]; then
    echo "warning: $out/$name has sha256 $got, recorded $h264_sha256 (another GPU or driver encodes differently)" >&2
  fi
}

# The scale and encode pipeline (no spaces inside an element's arguments: it is split into words as it is).
encode="fdsrc fd=0 blocksize=$((src_w * src_h * 3 / 2))
  ! rawvideoparse format=i420 width=$src_w height=$src_h framerate=24000/1001
  ! videoscale method=bilinear ! video/x-raw,width=1280,height=720
  ! videoconvert ! video/x-raw,format=NV12
  ! vah264enc rate-control=cqp qpi=22 qpp=24 key-int-max=60
  ! h264parse ! video/x-h264,stream-format=byte-stream,alignment=au
  ! filesink location=/out/$name.part"
dev_image_id=$(docker image inspect --format '{{.Id}}' "$dev_image")
recipe=$(printf 'source %s\ndecoder %s\nrequirements %s\nimage %s\ndev-image %s\nencode %s\n' "$src_sha256" \
  "$(sha256sum "$here/decode-tracker-clip.py" | cut -d' ' -f1)" \
  "$(sha256sum "$here/tracker-clip-requirements.txt" | cut -d' ' -f1)" "$image" "$dev_image_id" \
  "$(echo $encode)")

if [ -f "$out/$name" ] && [ "$(cat "$out/$name.recipe" 2>/dev/null)" = "$recipe" ]; then
  echo "$out/$name is up to date"
  check_h264
  exit 0
fi

mkdir -p "$out" "$work"
rm -f "$out/$name" "$out/$name.recipe"
clip="$work/DiagonalCrosswalkYongeDundas.webm"
if [ ! -f "$clip" ] || [ "$(sha256sum "$clip" | cut -d' ' -f1)" != "$src_sha256" ]; then
  rm -f "$clip" "$clip.part"
  curl --fail --location --proto =https --proto-redir =https --silent --show-error --connect-timeout 30 \
    --max-time 900 --retry 2 \
    --user-agent 'redstream-tests (https://commons.wikimedia.org)' -o "$clip.part" "$url"
  got=$(sha256sum "$clip.part" | cut -d' ' -f1)
  if [ "$got" != "$src_sha256" ]; then
    rm -f "$clip.part"
    echo "error: $url has sha256 $got, expected $src_sha256" >&2
    exit 1
  fi
  mv "$clip.part" "$clip"
fi

gid () { getent group "$1" | cut -d: -f3; }
render_gid=$(gid render)
video_gid=$(gid video)
if [ -z "$render_gid" ] || [ -z "$video_gid" ]; then
  echo "error: no render or video group on the host" >&2
  exit 1
fi

# PyAV into the venv, with network access. The cached venv is trusted by its stamp: it is rebuilt (installed with
# --require-hashes) whenever the requirements or the image change, since the stamp holds a sha256 of both.
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e IMAGE="$image" -v "$work":/work \
  -v "$here/tracker-clip-requirements.txt":/in/requirements.txt:ro \
  "$image" sh -euc '
    stamp=$({ cat /in/requirements.txt; echo "$IMAGE"; } | sha256sum | cut -d" " -f1)
    if [ "$(cat /work/venv/.stamp 2>/dev/null)" != "$stamp" ]; then
      rm -rf /work/venv
      python -m venv /work/venv
      /work/venv/bin/pip install -q --disable-pip-version-check --cache-dir /work/pip-cache --no-deps \
        --only-binary :all: --require-hashes -r /in/requirements.txt
      echo "$stamp" > /work/venv/.stamp
    fi'

# Decode (PyAV, raw I420 at the clip's size) | scale to 1280x720 and encode (vah264enc, constant QP), both without
# network access. A pipe hides the decoder's exit status (the encoder would just see a short stream), so the decoder
# writes it to a file.
rm -f "$work/decode.status"
set +e
(
  docker run --rm -i --network none --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$work":/work:ro \
    -v "$clip":/in/clip.webm:ro -v "$here/decode-tracker-clip.py":/in/decode-tracker-clip.py:ro \
    "$image" /work/venv/bin/python -I /in/decode-tracker-clip.py /in/clip.webm "$src_w" "$src_h"
  echo $? > "$work/decode.status"
) | docker run --rm -i --network none --user "$(id -u):$(id -g)" -e HOME=/tmp --device=/dev/dri \
  --group-add "$render_gid" --group-add "$video_gid" -v "$out":/out "$dev_image" \
  gst-launch-1.0 -q $encode
status=$?
set -e
if [ $status -ne 0 ] || [ "$(cat "$work/decode.status" 2>/dev/null)" != 0 ]; then
  rm -f "$out/$name.part"
  echo "error: the transcode failed" >&2
  exit 1
fi
mv "$out/$name.part" "$out/$name"
echo "$recipe" > "$out/$name.recipe"
echo "wrote $out/$name ($(wc -c < "$out/$name") bytes)"
check_h264
