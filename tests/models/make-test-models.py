#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Writes the small ONNX test models next to this script (deterministic: re-running gives identical bytes).

  scale2.onnx     images float[batch,3,height,width] -> output0 = images * 2.0 (batch, height and width
                  symbolic; the channel dim is fixed at 3)
  scale2-b1.onnx  the same with batch fixed to 1
  scale2-b4.onnx  the same with batch fixed to 4
  meancls.onnx    images float[batch,3,32,32] -> output0 float[batch,3] = the mean of each channel (ReduceMean
                  over height and width): a classifier whose class scores are the channel means, so with
                  net-scale-factor 1/255 a pure red crop is class 0 at 1.0
  meancls2.onnx   the same input -> output0 = the channel means, output1 = 1 - the channel means: two output
                  layers (two classifier attributes)
  fixeddet.onnx   images float[batch,3,64,64] -> output0 float[batch,6,8]: a detector in the YOLO layout
                  [4 + num_classes, anchors] (rows cx, cy, w, h in network pixels, then the scores of classes 0
                  and 1) whose output is a constant, the same for every image (FIXEDDET_BOXES):

                    anchor  cx  cy   w   h  class 0  class 1  detection: class, confidence, left, top, width, height
                         0  16  16  16  16  0.9      0.05     0, 0.9,   8,  8, 16, 16
                         2  48  16  20  12  0.05     0.8      1, 0.8,  38, 10, 20, 12
                         3  60  60  16  16  0.85     0.05     0, 0.85, 52, 52, 16, 16 (right and bottom edges
                                                              outside the network input: clipped to 52, 52, 12, 12)
                         5   4  48  16  20  0.75     0        0, 0.75, -4, 38, 16, 20 (left edge outside: clipped
                                                              to 0, 38, 12, 20)
                         6  58  30  12   8  0        0.7      1, 0.7,  52, 26, 12,  8 (right edge exactly at 64:
                                                              not clipped)
                         7  44  44  24  16  0.05     0.6      1, 0.6,  32, 36, 24, 16

                  The other anchors are all zero. With num-detected-classes 2, the built-in YOLO parser and the
                  default pre-cluster-threshold (0.2) give these six detections; every other score is 0.05 or
                  less, and no two boxes overlap, so NMS keeps them all.

Every output depends on the input through real ops: a multiply (scale2), reductions (meancls, meancls2), and for
fixeddet the constant plus Relu(-|sum of the image|), which is 0, so an inference engine can neither alias an
output to the input nor fold the graph into a constant without the batch dimension. (Not 0 x the sum: MIGraphX
folds that, and at batch 1 the program then returns a literal and has no output parameter.) Needs the `onnx`
package (see README.md for the pinned command).
"""
import os

import onnx
from onnx import TensorProto, helper

OPSET = 17
IR_VERSION = 8  # the lowest IR version that opset 17 allows: readable by older runtimes too


def finish(graph):
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", OPSET)],
                              producer_name="redstream make-test-models.py", ir_version=IR_VERSION)
    onnx.checker.check_model(model, full_check=True)
    return model


def make_scale2(batch):
    shape = [batch, 3, "height", "width"]
    graph = helper.make_graph(
        [helper.make_node("Mul", ["images", "scale"], ["output0"])],
        "scale2",
        [helper.make_tensor_value_info("images", TensorProto.FLOAT, shape)],
        [helper.make_tensor_value_info("output0", TensorProto.FLOAT, shape)],
        initializer=[helper.make_tensor("scale", TensorProto.FLOAT, [], [2.0])],
    )
    return finish(graph)


def make_meancls(two_outputs):
    # opset 17's ReduceMean takes its axes as an attribute
    nodes = [helper.make_node("ReduceMean", ["images"], ["output0"], axes=[2, 3], keepdims=0)]
    outputs = [helper.make_tensor_value_info("output0", TensorProto.FLOAT, ["batch", 3])]
    initializers = []
    if two_outputs:
        nodes.append(helper.make_node("Sub", ["one", "output0"], ["output1"]))
        outputs.append(helper.make_tensor_value_info("output1", TensorProto.FLOAT, ["batch", 3]))
        initializers.append(helper.make_tensor("one", TensorProto.FLOAT, [], [1.0]))
    graph = helper.make_graph(
        nodes,
        "meancls2" if two_outputs else "meancls",
        [helper.make_tensor_value_info("images", TensorProto.FLOAT, ["batch", 3, 32, 32])],
        outputs,
        initializer=initializers,
    )
    return finish(graph)


# fixeddet's boxes: anchor -> (cx, cy, w, h, score of class 0, score of class 1); the other anchors are zero.
FIXEDDET_BOXES = {
    0: (16, 16, 16, 16, 0.9, 0.05),
    2: (48, 16, 20, 12, 0.05, 0.8),
    3: (60, 60, 16, 16, 0.85, 0.05),
    5: (4, 48, 16, 20, 0.75, 0.0),
    6: (58, 30, 12, 8, 0.0, 0.7),
    7: (44, 44, 24, 16, 0.05, 0.6),
}
FIXEDDET_CLASSES = 2
FIXEDDET_ANCHORS = 8


def make_fixeddet():
    rows = 4 + FIXEDDET_CLASSES
    table = [0.0] * (rows * FIXEDDET_ANCHORS)
    for anchor, values in FIXEDDET_BOXES.items():
        for r, v in enumerate(values):
            table[r * FIXEDDET_ANCHORS + anchor] = float(v)
    # each image's sum [batch, 1, 1, 1] -> [batch, 1, 1], to 0 by Relu(-|sum|), plus the boxes [1, rows, anchors]:
    # broadcast to [batch, rows, anchors]
    graph = helper.make_graph(
        [
            helper.make_node("ReduceSum", ["images", "image_axes"], ["sum"], keepdims=1),
            helper.make_node("Reshape", ["sum", "per_image"], ["sum3"]),
            helper.make_node("Abs", ["sum3"], ["abs3"]),
            helper.make_node("Neg", ["abs3"], ["neg3"]),
            helper.make_node("Relu", ["neg3"], ["zero3"]),
            helper.make_node("Add", ["zero3", "boxes"], ["output0"]),
        ],
        "fixeddet",
        [helper.make_tensor_value_info("images", TensorProto.FLOAT, ["batch", 3, 64, 64])],
        [helper.make_tensor_value_info("output0", TensorProto.FLOAT, ["batch", rows, FIXEDDET_ANCHORS])],
        initializer=[
            helper.make_tensor("image_axes", TensorProto.INT64, [3], [1, 2, 3]),
            helper.make_tensor("per_image", TensorProto.INT64, [3], [-1, 1, 1]),
            helper.make_tensor("boxes", TensorProto.FLOAT, [1, rows, FIXEDDET_ANCHORS], table),
        ],
    )
    return finish(graph)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    models = (
        ("scale2.onnx", make_scale2("batch")),
        ("scale2-b1.onnx", make_scale2(1)),
        ("scale2-b4.onnx", make_scale2(4)),
        ("meancls.onnx", make_meancls(False)),
        ("meancls2.onnx", make_meancls(True)),
        ("fixeddet.onnx", make_fixeddet()),
    )
    for name, model in models:
        path = os.path.join(here, name)
        with open(path, "wb") as f:
            f.write(model.SerializeToString(deterministic=True))
        print("wrote", path)


if __name__ == "__main__":
    main()
