#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exports Ultralytics YOLO11n-cls to ONNX for the rdsinfer classifier tests and writes their reference data. Run by
fetch-yolo11n-cls.sh inside a throwaway container with the pins in export-requirements.txt, after
fetch-yolo11n.sh has written the street photo and its detections; not meant to be run directly.

Usage: export-yolo11n-cls.py <work-dir> <out-dir>

Writes to <out-dir>:
- yolo11n-cls.onnx: dynamic batch/height/width, opset 17, `date` metadata removed so the bytes are reproducible, and
  a Softmax (axis 1) appended to the output, sha256 checked. Ultralytics' export outputs the logits (its predictor
  applies the softmax); an nvinfer-style classifier takes the output as probabilities (the built-in parser's argmax
  and classifier-threshold), so the model gets the softmax itself. Output `output0 [batch, 1000]`;
- imagenet-labels.txt: the 1000 class names on one line, ';'-separated, in class-index order (the classifier label
  file form: one line per output layer);
- street-cls-ref.json: the vehicle detections of street-ref.json (car, motorcycle, bus, truck), each as three boxes
  (BOX_SCALES: as detected, and grown about its centre), each box with its crop of
  street-1280x720.rgba (the box aligned as rdsinfer crops it: origin floored and rounded up to even, size floored
  and rounded down to even, clipped to the image) and Ultralytics predict on that crop (imgsz 224: shorter side
  resized to 224, centre crop, RGB scaled to [0, 1]): top-1 class, label and probability, and the top 5; and the
  top two classes of yolo11n-cls.onnx in ONNX Runtime on the crop as the rds-test2 config preprocesses it (stretched to
  224x224, bilinear, RGB scaled to [0, 1]);
- cls-parity-in.f32 / cls-parity-out.f32: raw little-endian float32. The input is the first vehicle crop through
  Ultralytics' own classify transform as a 1x3x224x224 RGB tensor; the output is ONNX Runtime's (CPU, one thread)
  1x1000 result on it.
Ultralytics' own export (logits, metadata stripped) is kept in <work-dir> as yolo11n-cls-logits.onnx: the reference
predictions run on it, as Ultralytics would.
The derived files' sha256 are compared with DERIVED_SHA256, the values with the pinned toolchain on an x86-64 CPU; a
mismatch is only a warning, since ONNX Runtime's results can differ in the last bits on another CPU.
YOLO11n-cls is AGPL-3.0: the outputs stay in the git-ignored cache.
"""
import hashlib
import json
import math
import os
import shutil
import sys
import tempfile
import urllib.request

PT_URL = "https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n-cls.pt"
PT_SHA256 = "c62d41bf9625777760018bf914d2e6cd472420ccd01706d97a61cb6c82502bd7"
ONNX_SHA256 = "24a6b3b8b4be388e25aabd840d03b4a08b045496530e222cdaac6557a049a33b"
NUM_CLASSES = 1000
IMGSZ = 224
STREET_W, STREET_H = 1280, 720
# COCO class ids of the vehicles the classifier runs on: car, motorcycle, bus, truck
VEHICLES = (2, 3, 5, 7)
# The photo has only 5 vehicles (YOLO11n finds no other above confidence 0.05), so each gives three crops: its box,
# and the box grown about its centre by these factors (clipped to the image), as a looser detector would draw it.
BOX_SCALES = (1.0, 1.15, 1.3)
DERIVED_SHA256 = {
    "imagenet-labels.txt": "6750e1f95e38ac5e68296941a8d8cef29270a07162a4688e399ee2fef7fbdedf",
    "street-cls-ref.json": "558d97e42c3f125eb12dc9d15fdb8d5613cddd2e4fd93f95586d761587225bba",
    "cls-parity-in.f32": "790903d030e5ed95af6675a9a4df450fde1f712309f89911235eb6bbf0349f92",
    "cls-parity-out.f32": "061022fe280da4d120df48291c1fe077085797b2902766a766b31544fc662499",
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(url, path, expected):
    if os.path.exists(path) and sha256(path) == expected:
        return path
    print("downloading", url, flush=True)
    req = urllib.request.Request(url, headers={"User-Agent": "redstream-test-fetch/1.0 (fetch-yolo11n-cls.sh)"})
    with urllib.request.urlopen(req, timeout=60) as r, open(path + ".part", "wb") as f:
        shutil.copyfileobj(r, f)
    got = sha256(path + ".part")
    if got != expected:
        sys.exit(f"{os.path.basename(path)} sha256 mismatch:\n  expected {expected}\n  got      {got}")
    os.replace(path + ".part", path)
    return path


def write_atomic(path, data):
    mode = "w" if isinstance(data, str) else "wb"
    with open(path + ".part", mode) as f:
        f.write(data)
    os.replace(path + ".part", path)


def export_onnx(work, out):
    pt = fetch(PT_URL, os.path.join(work, "yolo11n-cls.pt"), PT_SHA256)

    import onnx
    from ultralytics import YOLO

    # Ultralytics writes the export next to the weights: do it in a scratch directory.
    with tempfile.TemporaryDirectory(dir=work) as tmp:
        local_pt = os.path.join(tmp, "yolo11n-cls.pt")
        shutil.copyfile(pt, local_pt)
        model = YOLO(local_pt)
        names = model.names
        exported = model.export(format="onnx", imgsz=IMGSZ, opset=17, dynamic=True, simplify=True)

        proto = onnx.load(exported)
        props = [p for p in proto.metadata_props if p.key != "date"]
        del proto.metadata_props[:]
        proto.metadata_props.extend(props)
        logits_onnx = os.path.join(tmp, "yolo11n-cls-logits.onnx")
        onnx.save(proto, logits_onnx)

        # output0 = Softmax (logits, axis 1): the node that made output0 now makes "logits"
        out_name = proto.graph.output[0].name
        producers = [n for n in proto.graph.node if out_name in n.output]
        if out_name != "output0" or len(producers) != 1:
            sys.exit(f"unexpected graph output {out_name!r}")
        producers[0].output[list(producers[0].output).index(out_name)] = "logits"
        proto.graph.node.append(onnx.helper.make_node("Softmax", ["logits"], [out_name], name="softmax_out", axis=1))
        onnx.checker.check_model(proto)
        tmp_onnx = os.path.join(tmp, "yolo11n-cls.onnx.stripped")
        onnx.save(proto, tmp_onnx)

        got = sha256(tmp_onnx)
        if got != ONNX_SHA256:
            print(f"yolo11n-cls.onnx sha256 mismatch (the export pins or the toolchain changed; re-pin "
                  f"deliberately):\n  expected {ONNX_SHA256}\n  got      {got}", file=sys.stderr)
            sys.exit(1)
        shutil.copyfile(logits_onnx, os.path.join(work, "yolo11n-cls-logits.onnx.part"))
        os.replace(os.path.join(work, "yolo11n-cls-logits.onnx.part"), os.path.join(work, "yolo11n-cls-logits.onnx"))
        shutil.copyfile(tmp_onnx, os.path.join(out, "yolo11n-cls.onnx.part"))
        os.replace(os.path.join(out, "yolo11n-cls.onnx.part"), os.path.join(out, "yolo11n-cls.onnx"))

    if sorted(names) != list(range(NUM_CLASSES)):
        sys.exit(f"unexpected class names: {names}")
    for i in range(NUM_CLASSES):
        if not names[i] or ";" in names[i] or "\n" in names[i] or names[i] != names[i].strip():
            sys.exit(f"class name {i} {names[i]!r} can't go in a ';'-separated label file")
    write_atomic(os.path.join(out, "imagenet-labels.txt"), ";".join(names[i] for i in range(NUM_CLASSES)) + "\n")
    print("wrote", os.path.join(out, "yolo11n-cls.onnx"), "and imagenet-labels.txt")


def aligned_crop(left, top, width, height):
    """The crop rdsinfer takes of a box in secondary mode: per axis the origin floored and rounded up to even, the
    size floored and rounded down to even (at least 2), then clipped to the image (and the size made even again).
    None when nothing is left."""
    def axis(pos, size, limit):
        if size < 1:
            return None
        o = math.floor(pos)
        o += o % 2
        s = max(math.floor(size) // 2 * 2, 2)
        a, b = max(o, 0), min(o + s, limit)
        s = (b - a) // 2 * 2
        return (a, s) if s >= 2 else None

    x, y = axis(left, width, STREET_W), axis(top, height, STREET_H)
    if x is None or y is None:
        return None
    return x[0], y[0], x[1], y[1]


def write_reference(work, out):
    import cv2
    import numpy as np
    import onnxruntime as ort
    from PIL import Image
    from ultralytics import YOLO
    from ultralytics.data.augment import classify_transforms

    onnx_path = os.path.join(out, "yolo11n-cls.onnx")
    with open(os.path.join(out, "street-ref.json")) as f:
        det_ref = json.load(f)
    with open(os.path.join(out, "street-1280x720.rgba"), "rb") as f:
        street = Image.frombytes("RGBA", (STREET_W, STREET_H), f.read()).convert("RGB")
    if (det_ref["width"], det_ref["height"]) != (STREET_W, STREET_H):
        sys.exit("street-ref.json is not for a 1280x720 image")

    # ONNX Runtime on CPU with one thread, so the output doesn't depend on how the work is split.
    opts = ort.SessionOptions()
    opts.intra_op_num_threads = 1
    opts.inter_op_num_threads = 1
    sess = ort.InferenceSession(onnx_path, opts, providers=["CPUExecutionProvider"])

    def classify(tensor):
        return sess.run(None, {sess.get_inputs()[0].name: tensor.astype(np.float32)})[0]

    # 1. Ultralytics predict on each vehicle's crop: its own export through ONNX Runtime (the model rdsinfer runs, but
    # for the softmax, which Ultralytics' predictor applies itself). And the same crop as the rds-test2 config gives
    # it to the model (maintain-aspect-ratio=0: stretched to 224x224, bilinear with half-pixel centres, RGB / 255)
    # through yolo11n-cls.onnx.
    vehicles = []
    for det in det_ref["objects"]:
        if det["class_id"] not in VEHICLES:
            continue
        cx, cy = det["left"] + det["width"] / 2, det["top"] + det["height"] / 2
        for scale in BOX_SCALES:
            x1 = max(cx - det["width"] * scale / 2, 0.0)
            y1 = max(cy - det["height"] * scale / 2, 0.0)
            x2 = min(cx + det["width"] * scale / 2, float(STREET_W))
            y2 = min(cy + det["height"] * scale / 2, float(STREET_H))
            vehicles.append(dict(det, scale=scale, left=round(x1, 2), top=round(y1, 2), width=round(x2 - x1, 2),
                                 height=round(y2 - y1, 2)))
    model = YOLO(os.path.join(work, "yolo11n-cls-logits.onnx"), task="classify")
    objects = []
    for det in vehicles:
        crop = aligned_crop(det["left"], det["top"], det["width"], det["height"])
        if crop is None:
            continue
        x, y, w, h = crop
        image = street.crop((x, y, x + w, y + h))
        probs = model.predict(image, imgsz=IMGSZ, device="cpu", verbose=False)[0].probs
        top5 = [{"class_id": int(c), "label": model.names[int(c)], "prob": round(float(p), 5)}
                for c, p in zip(probs.top5, probs.top5conf.tolist())]
        stretched = cv2.resize(np.asarray(image), (IMGSZ, IMGSZ), interpolation=cv2.INTER_LINEAR)
        p = classify(stretched.transpose(2, 0, 1)[None] / 255.0)[0]
        order = p.argsort()[::-1]
        stretch = {"class_id": int(order[0]), "label": model.names[int(order[0])], "prob": round(float(p[order[0]]), 5),
                   "second_class_id": int(order[1]), "second_prob": round(float(p[order[1]]), 5)}
        objects.append({"class_id": det["class_id"], "label": det["label"], "confidence": det["confidence"],
                        "scale": det["scale"], "left": det["left"], "top": det["top"], "width": det["width"], "height": det["height"],
                        "crop": [x, y, w, h], "top1": top5[0], "top5": top5, "stretch_top1": stretch})
    if not objects:
        sys.exit("no vehicle in street-ref.json")
    ref = {
        "image": "street-1280x720.rgba", "width": STREET_W, "height": STREET_H, "detections": "street-ref.json",
        "predict": {"model": "yolo11n-cls.onnx without the final Softmax (Ultralytics' export)", "imgsz": IMGSZ,
                    "transform": f"shorter side resized to {IMGSZ} (bilinear), centre crop {IMGSZ}x{IMGSZ}, "
                                 "RGB scaled to [0, 1]"},
        "stretch": {"model": "yolo11n-cls.onnx", "runtime": "ONNX Runtime, CPU, one thread",
                    "transform": f"crop resized to {IMGSZ}x{IMGSZ} (OpenCV INTER_LINEAR), RGB scaled to [0, 1]"},
        "vehicle_classes": list(VEHICLES),
        "box_scales": list(BOX_SCALES),
        "objects": objects,
    }
    write_atomic(os.path.join(out, "street-cls-ref.json"), json.dumps(ref, indent=1) + "\n")

    # 2. The engine parity pair: the first vehicle crop through Ultralytics' classify transform.
    x, y, w, h = objects[0]["crop"]
    tensor = classify_transforms(IMGSZ)(street.crop((x, y, x + w, y + h)))[None].numpy().astype(np.float32)
    output = classify(tensor)
    if tensor.shape != (1, 3, IMGSZ, IMGSZ) or output.shape != (1, NUM_CLASSES):
        sys.exit(f"unexpected shapes: input {tensor.shape}, output {output.shape}")
    write_atomic(os.path.join(out, "cls-parity-in.f32"), tensor.astype("<f4").tobytes())
    write_atomic(os.path.join(out, "cls-parity-out.f32"), output.astype("<f4").tobytes())

    print(f"wrote the reference data ({len(objects)} vehicles in street-cls-ref.json):")
    for o in objects:
        print(f"  {o['label']:<10} x{o['scale']:<4} crop {o['crop']}: {o['top1']['label']} {o['top1']['prob']:.4f}, stretched "
              f"{o['stretch_top1']['label']} {o['stretch_top1']['prob']:.4f}")
    print(f"  parity tensor: top-1 {int(output.argmax())} ({model.names[int(output.argmax())]}) "
          f"{float(output.max()):.4f}, sum {float(output.sum()):.4f}")
    for name, expected in DERIVED_SHA256.items():
        got = sha256(os.path.join(out, name))
        note = "" if got == expected else "  (differs from the recorded hash: another CPU or toolchain?)"
        print(f"  {got}  {name}{note}")


def main():
    work, out = sys.argv[1], sys.argv[2]
    for name in ("street-1280x720.rgba", "street-ref.json"):
        if not os.path.exists(os.path.join(out, name)):
            sys.exit(f"no {name} in {out}: run fetch-yolo11n.sh first")
    onnx_path = os.path.join(out, "yolo11n-cls.onnx")
    if not (os.path.exists(onnx_path) and sha256(onnx_path) == ONNX_SHA256
            and os.path.exists(os.path.join(out, "imagenet-labels.txt"))
            and os.path.exists(os.path.join(work, "yolo11n-cls-logits.onnx"))):
        export_onnx(work, out)
    write_reference(work, out)


if __name__ == "__main__":
    main()
