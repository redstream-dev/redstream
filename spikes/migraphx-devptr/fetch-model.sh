#!/bin/sh
# Download the public test model into the git-ignored cache.
set -eu
mkdir -p .cache/models
url=https://github.com/onnx/models/raw/main/validated/vision/classification/mobilenet/model/mobilenetv2-12.onnx
[ -f .cache/models/mobilenetv2-12.onnx ] || curl -fsSL -o .cache/models/mobilenetv2-12.onnx "$url"
sha256sum .cache/models/mobilenetv2-12.onnx
