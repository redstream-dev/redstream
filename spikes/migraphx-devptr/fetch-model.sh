#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Download the public test model into the git-ignored cache and verify its pinned sha256.
set -eu
mkdir -p .cache/models
url=https://github.com/onnx/models/raw/main/validated/vision/classification/mobilenet/model/mobilenetv2-12.onnx
sha=c0c3f76d93fa3fd6580652a45618618a220fced18babf65774ed169de0432ad5
[ -f .cache/models/mobilenetv2-12.onnx ] || curl -fsSL -o .cache/models/mobilenetv2-12.onnx "$url"
echo "$sha  .cache/models/mobilenetv2-12.onnx" | sha256sum -c -
