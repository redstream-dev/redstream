#!/bin/sh
# Downloads Ultralytics YOLO11n and exports it to .cache/models/yolo11n.onnx (+ coco-labels.txt) for the rdsinfer
# tests. Runs on the host and needs Docker and network access. The export runs in a throwaway python:3.12-slim
# container with the pins in export-requirements.txt; the venv and pip cache stay in .cache/yolo-export so re-runs
# are fast. Outputs whose sha256 already matches are not redone.
#
# YOLO11n is AGPL-3.0: the model is fetched and exported on your machine and never committed.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out="$root/.cache/models"
work="$root/.cache/yolo-export"
onnx_sha256=20f781e2971c3b03869f02197d9201126ed4a83bd2bfec2b159cf512cb7370e2
# Pinned by digest: the export hash depends on the toolchain in the image too.
image=python:3.12-slim@sha256:a0f57064cbdcd6285fa45f51237ab0b35ea8ebaed06a1f5973e89e19dfb86704

if [ -f "$out/yolo11n.onnx" ] && [ -f "$out/coco-labels.txt" ] \
    && [ "$(sha256sum "$out/yolo11n.onnx" | cut -d' ' -f1)" = "$onnx_sha256" ] \
    && [ "$(wc -l < "$out/coco-labels.txt")" -eq 80 ]; then
  echo "$out/yolo11n.onnx is up to date"
  exit 0
fi

mkdir -p "$out" "$work"
# PYTHONHASHSEED is fixed because onnxslim's shape inference fills graph.value_info in an order (and to an extent)
# that depends on Python's hash randomisation: unseeded exports differ from run to run, seeded ones are identical.
# The venv is rebuilt whenever the requirements or the image change (its stamp holds a sha256 of both).
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e YOLO_CONFIG_DIR=/tmp/ultralytics -e PYTHONHASHSEED=0 \
  -e YOLO_AUTOINSTALL=false -e IMAGE="$image" \
  -v "$out":/out -v "$work":/work \
  -v "$here/export-requirements.txt":/in/export-requirements.txt:ro \
  -v "$here/export-yolo11n.py":/in/export-yolo11n.py:ro \
  "$image" sh -euc '
    stamp=$({ cat /in/export-requirements.txt; echo "$IMAGE"; } | sha256sum | cut -d" " -f1)
    if [ "$(cat /work/venv/.stamp 2>/dev/null)" != "$stamp" ]; then
      rm -rf /work/venv
      python -m venv /work/venv
      /work/venv/bin/pip install -q --disable-pip-version-check --cache-dir /work/pip-cache --no-deps \
        --extra-index-url https://download.pytorch.org/whl/cpu -r /in/export-requirements.txt
      echo "$stamp" > /work/venv/.stamp
    fi
    /work/venv/bin/python /in/export-yolo11n.py /work /out
  '
