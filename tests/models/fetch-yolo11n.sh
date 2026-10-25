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

if [ -f "$out/yolo11n.onnx" ] && [ -f "$out/coco-labels.txt" ] \
    && [ "$(sha256sum "$out/yolo11n.onnx" | cut -d' ' -f1)" = "$onnx_sha256" ] \
    && [ "$(wc -l < "$out/coco-labels.txt")" -eq 80 ]; then
  echo "$out/yolo11n.onnx is up to date"
  exit 0
fi

mkdir -p "$out" "$work"
# PYTHONHASHSEED is fixed because onnxslim's shape inference fills graph.value_info in an order (and to an extent)
# that depends on Python's hash randomisation: unseeded exports differ from run to run, seeded ones are identical.
# The venv is rebuilt whenever the requirements change (its stamp holds their sha256).
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e YOLO_CONFIG_DIR=/tmp/ultralytics -e PYTHONHASHSEED=0 \
  -v "$out":/out -v "$work":/work \
  -v "$here/export-requirements.txt":/in/export-requirements.txt:ro \
  -v "$here/export-yolo11n.py":/in/export-yolo11n.py:ro \
  python:3.12-slim sh -euc '
    req_sha=$(sha256sum /in/export-requirements.txt | cut -d" " -f1)
    if [ "$(cat /work/venv/.stamp 2>/dev/null)" != "$req_sha" ]; then
      rm -rf /work/venv
      python -m venv /work/venv
      /work/venv/bin/pip install -q --disable-pip-version-check --cache-dir /work/pip-cache --no-deps \
        --extra-index-url https://download.pytorch.org/whl/cpu -r /in/export-requirements.txt
      echo "$req_sha" > /work/venv/.stamp
    fi
    /work/venv/bin/python /in/export-yolo11n.py /work /out
  '
