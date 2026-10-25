#!/usr/bin/env python3
"""Exports Ultralytics YOLO11n to ONNX for the rdsinfer tests. Run by fetch-yolo11n.sh inside a throwaway container
with the pins in export-requirements.txt; not meant to be run directly.

Usage: export-yolo11n.py <work-dir> <out-dir>

Writes <out-dir>/yolo11n.onnx (dynamic batch/height/width, opset 17, `date` metadata removed so the bytes are
reproducible, sha256 checked) and <out-dir>/coco-labels.txt (one class name per line, in class-index order).
YOLO11n is AGPL-3.0: the outputs stay in the git-ignored cache.
"""
import hashlib
import os
import shutil
import sys
import tempfile
import urllib.request

PT_URL = "https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n.pt"
PT_SHA256 = "0ebbc80d4a7680d14987a577cd21342b65ecfd94632bd9a8da63ae6417644ee1"
ONNX_SHA256 = "20f781e2971c3b03869f02197d9201126ed4a83bd2bfec2b159cf512cb7370e2"
NUM_CLASSES = 80


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_pt(work):
    pt = os.path.join(work, "yolo11n.pt")
    if os.path.exists(pt) and sha256(pt) == PT_SHA256:
        return pt
    print("downloading", PT_URL, flush=True)
    urllib.request.urlretrieve(PT_URL, pt + ".part")
    got = sha256(pt + ".part")
    if got != PT_SHA256:
        sys.exit(f"yolo11n.pt sha256 mismatch:\n  expected {PT_SHA256}\n  got      {got}")
    os.replace(pt + ".part", pt)
    return pt


def main():
    work, out = sys.argv[1], sys.argv[2]
    pt = fetch_pt(work)

    import onnx
    from ultralytics import YOLO

    # Ultralytics writes the export next to the weights: do it in a scratch directory.
    with tempfile.TemporaryDirectory(dir=work) as tmp:
        local_pt = os.path.join(tmp, "yolo11n.pt")
        shutil.copyfile(pt, local_pt)
        model = YOLO(local_pt)
        names = model.names
        exported = model.export(format="onnx", imgsz=640, opset=17, dynamic=True, simplify=True)

        proto = onnx.load(exported)
        props = [p for p in proto.metadata_props if p.key != "date"]
        del proto.metadata_props[:]
        proto.metadata_props.extend(props)
        tmp_onnx = os.path.join(tmp, "yolo11n.onnx.stripped")
        onnx.save(proto, tmp_onnx)

        got = sha256(tmp_onnx)
        if got != ONNX_SHA256:
            print(f"yolo11n.onnx sha256 mismatch (the export pins or the toolchain changed; re-pin deliberately):\n"
                  f"  expected {ONNX_SHA256}\n  got      {got}", file=sys.stderr)
            sys.exit(1)
        shutil.copyfile(tmp_onnx, os.path.join(out, "yolo11n.onnx.part"))
        os.replace(os.path.join(out, "yolo11n.onnx.part"), os.path.join(out, "yolo11n.onnx"))

    if sorted(names) != list(range(NUM_CLASSES)):
        sys.exit(f"unexpected class names: {names}")
    with open(os.path.join(out, "coco-labels.txt.part"), "w") as f:
        f.write("".join(names[i] + "\n" for i in range(NUM_CLASSES)))
    os.replace(os.path.join(out, "coco-labels.txt.part"), os.path.join(out, "coco-labels.txt"))
    print("wrote", os.path.join(out, "yolo11n.onnx"), "and coco-labels.txt")


if __name__ == "__main__":
    main()
