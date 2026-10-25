#!/usr/bin/env python3
"""Exports Ultralytics YOLO11n to ONNX for the rdsinfer tests and writes their reference data. Run by
fetch-yolo11n.sh inside a throwaway container with the pins in export-requirements.txt; not meant to be run directly.

Usage: export-yolo11n.py <work-dir> <out-dir>

Writes to <out-dir>:
- yolo11n.onnx: dynamic batch/height/width, opset 17, `date` metadata removed so the bytes are reproducible,
  sha256 checked;
- coco-labels.txt: one class name per line, in class-index order;
- street-1280x720.rgba: the public-domain street photo (STREET_URL, sha256 checked) as raw RGBA, resized to fit
  1280x720 (aspect ratio kept, Pillow Lanczos), top-left, padded with opaque black;
- street-ref.json: Ultralytics predict on that image with yolo11n.onnx (conf 0.25, NMS IoU 0.45, imgsz 640):
  class, confidence and box in 1280x720 pixels;
- yolo-parity-in.f32 / yolo-parity-out.f32: raw little-endian float32. The input is that image through Ultralytics'
  own letterbox (640x640, centred, padded with 114) as a 1x3x640x640 RGB tensor scaled to [0, 1]; the output is
  ONNX Runtime's (CPU, one thread) 1x84x8400 result on it.
The derived files' sha256 are compared with DERIVED_SHA256, the values with the pinned toolchain on an x86-64 CPU; a
mismatch is only a warning, since ONNX Runtime's results can differ in the last bits on another CPU.
YOLO11n is AGPL-3.0: the outputs stay in the git-ignored cache.
"""
import hashlib
import json
import os
import shutil
import sys
import tempfile
import urllib.request

PT_URL = "https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n.pt"
PT_SHA256 = "0ebbc80d4a7680d14987a577cd21342b65ecfd94632bd9a8da63ae6417644ee1"
ONNX_SHA256 = "20f781e2971c3b03869f02197d9201126ed4a83bd2bfec2b159cf512cb7370e2"
NUM_CLASSES = 80

# "Traffic and pedestrians on Calle Velasquez near Catedral San Nicolas de Bari in Porlamar, Venezuela" by Wilfredor,
# CC0 1.0 (Wikimedia Commons): 2367x1374 JPEG.
STREET_URL = ("https://upload.wikimedia.org/wikipedia/commons/9/93/Traffic_and_pedestrians_on_Calle_Vel%C3%A1squez_"
              "near_Catedral_San_Nicol%C3%A1s_de_Bari_in_Porlamar%2C_Venezuela.jpg")
STREET_SHA256 = "2c97420ab515eda95204755d114c64a7f8a4acf47e7670e104e85a9cea5feca3"
STREET_W, STREET_H = 1280, 720
CONF, IOU, IMGSZ = 0.25, 0.45, 640
DERIVED_SHA256 = {
    "street-1280x720.rgba": "ebaab900765ef653d7336e237d5ce1daa1cd0b37d02b7a23a3e970429affacd4",
    "street-ref.json": "fdf61dc33ff54c832120746e91a44367881cae29581fa023b246720668790f5a",
    "yolo-parity-in.f32": "43f0faad2edf5b161fef97304f6a5f53c16da4e1f9de761db35778e1e4d556b9",
    "yolo-parity-out.f32": "bac1b23e05f85f6c8665cdfb05258f8a140922c04e177168d5933687d78f3fb9",
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
    # Wikimedia refuses requests without a descriptive User-Agent.
    req = urllib.request.Request(url, headers={"User-Agent": "redstream-test-fetch/1.0 (fetch-yolo11n.sh)"})
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
    pt = fetch(PT_URL, os.path.join(work, "yolo11n.pt"), PT_SHA256)

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
    write_atomic(os.path.join(out, "coco-labels.txt"), "".join(names[i] + "\n" for i in range(NUM_CLASSES)))
    print("wrote", os.path.join(out, "yolo11n.onnx"), "and coco-labels.txt")


def write_reference(work, out):
    import numpy as np
    import onnxruntime as ort
    from PIL import Image
    from ultralytics import YOLO
    from ultralytics.data.augment import LetterBox

    onnx_path = os.path.join(out, "yolo11n.onnx")
    photo = fetch(STREET_URL, os.path.join(work, "street.jpg"), STREET_SHA256)

    # 1. The photo fitted into 1280x720: scaled by min(1280 / w, 720 / h) (2367x1374 -> 1240x720), top-left,
    # opaque black on the rest.
    src = Image.open(photo).convert("RGB")
    scale = min(STREET_W / src.width, STREET_H / src.height)
    size = (round(src.width * scale), round(src.height * scale))
    canvas = Image.new("RGBA", (STREET_W, STREET_H), (0, 0, 0, 255))
    canvas.paste(src.resize(size, Image.Resampling.LANCZOS).convert("RGBA"), (0, 0))
    write_atomic(os.path.join(out, "street-1280x720.rgba"), canvas.tobytes())

    # 2. Ultralytics predict on it (the ONNX model through ONNX Runtime, the model rdsinfer runs). Ultralytics drops
    # the alpha channel of a PIL image.
    model = YOLO(onnx_path, task="detect")
    result = model.predict(canvas, conf=CONF, iou=IOU, imgsz=IMGSZ, device="cpu", verbose=False)[0]
    objects = []
    for cls, conf, xyxy in zip(result.boxes.cls.tolist(), result.boxes.conf.tolist(), result.boxes.xyxy.tolist()):
        x1, y1, x2, y2 = xyxy
        objects.append({"class_id": int(cls), "label": result.names[int(cls)], "confidence": round(conf, 5),
                        "left": round(x1, 2), "top": round(y1, 2), "width": round(x2 - x1, 2),
                        "height": round(y2 - y1, 2)})
    ref = {
        "image": "street-1280x720.rgba", "width": STREET_W, "height": STREET_H,
        "source": {"url": STREET_URL, "sha256": STREET_SHA256, "size": [src.width, src.height],
                   "transform": f"Pillow LANCZOS resize to {size[0]}x{size[1]}, top-left, opaque black padding"},
        "predict": {"model": "yolo11n.onnx", "conf": CONF, "iou": IOU, "imgsz": IMGSZ},
        "objects": objects,
    }
    write_atomic(os.path.join(out, "street-ref.json"), json.dumps(ref, indent=1) + "\n")

    # 3. The engine parity pair: Ultralytics' letterbox on the BGR image, then RGB, CHW, / 255; ONNX Runtime on CPU
    # with one thread, so the output doesn't depend on how the work is split.
    bgr = np.ascontiguousarray(np.asarray(canvas.convert("RGB"))[:, :, ::-1])
    lb = LetterBox(new_shape=(IMGSZ, IMGSZ), auto=False, stride=32)(image=bgr)
    tensor = np.ascontiguousarray(lb[:, :, ::-1].transpose(2, 0, 1)[None]).astype(np.float32) / 255.0
    opts = ort.SessionOptions()
    opts.intra_op_num_threads = 1
    opts.inter_op_num_threads = 1
    sess = ort.InferenceSession(onnx_path, opts, providers=["CPUExecutionProvider"])
    output = sess.run(["output0"], {"images": tensor})[0]
    if output.shape != (1, 4 + NUM_CLASSES, 8400):
        sys.exit(f"unexpected output shape {output.shape}")
    write_atomic(os.path.join(out, "yolo-parity-in.f32"), tensor.astype("<f4").tobytes())
    write_atomic(os.path.join(out, "yolo-parity-out.f32"), output.astype("<f4").tobytes())

    print(f"wrote the reference data ({len(objects)} objects in street-ref.json):")
    for name, expected in DERIVED_SHA256.items():
        got = sha256(os.path.join(out, name))
        note = "" if got == expected else "  (differs from the recorded hash: another CPU or toolchain?)"
        print(f"  {got}  {name}{note}")


def main():
    work, out = sys.argv[1], sys.argv[2]
    onnx_path = os.path.join(out, "yolo11n.onnx")
    if not (os.path.exists(onnx_path) and sha256(onnx_path) == ONNX_SHA256
            and os.path.exists(os.path.join(out, "coco-labels.txt"))):
        export_onnx(work, out)
    write_reference(work, out)


if __name__ == "__main__":
    main()
