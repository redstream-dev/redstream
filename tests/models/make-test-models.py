#!/usr/bin/env python3
"""Writes the small ONNX test models next to this script (deterministic: re-running gives identical bytes).

  scale2.onnx     images float[batch,3,height,width] -> output0 = images * 2.0 (all dims symbolic)
  scale2-b1.onnx  the same with batch fixed to 1

A real multiply, so an inference engine cannot alias the output to the input. Needs the `onnx` package
(see README.md for the pinned command).
"""
import os

import onnx
from onnx import TensorProto, helper

OPSET = 17
IR_VERSION = 8  # the lowest IR version that opset 17 allows: readable by older runtimes too


def make_scale2(batch):
    shape = [batch, 3, "height", "width"]
    graph = helper.make_graph(
        [helper.make_node("Mul", ["images", "scale"], ["output0"])],
        "scale2",
        [helper.make_tensor_value_info("images", TensorProto.FLOAT, shape)],
        [helper.make_tensor_value_info("output0", TensorProto.FLOAT, shape)],
        initializer=[helper.make_tensor("scale", TensorProto.FLOAT, [], [2.0])],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", OPSET)],
                              producer_name="redstream make-test-models.py", ir_version=IR_VERSION)
    onnx.checker.check_model(model, full_check=True)
    return model


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    for name, batch in (("scale2.onnx", "batch"), ("scale2-b1.onnx", 1)):
        path = os.path.join(here, name)
        with open(path, "wb") as f:
            f.write(make_scale2(batch).SerializeToString(deterministic=True))
        print("wrote", path)


if __name__ == "__main__":
    main()
