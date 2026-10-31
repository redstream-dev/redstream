#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Downloads Ultralytics YOLO11n-cls and exports it to .cache/models/yolo11n-cls.onnx (+ imagenet-labels.txt) for the
# rdsinfer classifier tests and the rds-test2 sample, with their reference data: Ultralytics' top-1 on crops of the
# vehicles detected on the street photo, and an input/output pair from ONNX Runtime for the engine parity test (see
# README.md). Runs fetch-yolo11n.sh first (the detector, the photo and its detections). Runs on the host and needs
# Docker and network access: the same throwaway python:3.12-slim container, pins and venv (.cache/yolo-export) as
# fetch-yolo11n.sh. A model whose sha256 already matches is not exported again; the reference data is rewritten
# unless all of it is there.
#
# YOLO11n-cls is AGPL-3.0: the model and everything made with it are generated on your machine and never committed.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out="$root/.cache/models"
work="$root/.cache/yolo-export"
onnx_sha256=24a6b3b8b4be388e25aabd840d03b4a08b045496530e222cdaac6557a049a33b
# The same image as fetch-yolo11n.sh, so both share one venv.
image=python:3.12-slim@sha256:a0f57064cbdcd6285fa45f51237ab0b35ea8ebaed06a1f5973e89e19dfb86704

"$here/fetch-yolo11n.sh"

if [ -f "$out/yolo11n-cls.onnx" ] && [ -f "$out/imagenet-labels.txt" ] \
    && [ "$(sha256sum "$out/yolo11n-cls.onnx" | cut -d' ' -f1)" = "$onnx_sha256" ] \
    && [ -f "$out/street-cls-ref.json" ] && [ -f "$out/cls-parity-in.f32" ] && [ -f "$out/cls-parity-out.f32" ] \
    && [ -f "$work/yolo11n-cls-logits.onnx" ]; then
  echo "$out/yolo11n-cls.onnx and its reference data are up to date"
  exit 0
fi

mkdir -p "$out" "$work"
# As in fetch-yolo11n.sh: PYTHONHASHSEED fixed for a reproducible export, the venv rebuilt when the requirements or
# the image change.
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e YOLO_CONFIG_DIR=/tmp/ultralytics -e PYTHONHASHSEED=0 \
  -e YOLO_AUTOINSTALL=false -e IMAGE="$image" \
  -v "$out":/out -v "$work":/work \
  -v "$here/export-requirements.txt":/in/export-requirements.txt:ro \
  -v "$here/export-yolo11n-cls.py":/in/export-yolo11n-cls.py:ro \
  "$image" sh -euc '
    stamp=$({ cat /in/export-requirements.txt; echo "$IMAGE"; } | sha256sum | cut -d" " -f1)
    if [ "$(cat /work/venv/.stamp 2>/dev/null)" != "$stamp" ]; then
      rm -rf /work/venv
      python -m venv /work/venv
      /work/venv/bin/pip install -q --disable-pip-version-check --cache-dir /work/pip-cache --no-deps \
        --extra-index-url https://download.pytorch.org/whl/cpu -r /in/export-requirements.txt
      echo "$stamp" > /work/venv/.stamp
    fi
    /work/venv/bin/python /in/export-yolo11n-cls.py /work /out
  '
