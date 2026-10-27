# Design spikes

Throwaway experiments that de-risked the M1 design. They are not built by meson. Run them from the **repo root**,
**inside the dev container with the GPU attached** (see `.agents/skills/dev-container/SKILL.md`). Their Makefiles use
the container's ROCm paths, and their outputs go to the git-ignored `.cache/`.

| Spike | What it proves | Run |
|---|---|---|
| `rocdec-hip/` | rocDecode → HIP → owned surface is bit-exact vs stock VA, leak-free over 10k frames, copy <0.2 ms at 1080p | `tests/media/make-test-media.sh && make -C spikes/rocdec-hip && spikes/rocdec-hip/ref_crc .cache/media/ball-720p.h264 > .cache/ref.txt && spikes/rocdec-hip/rocdec_spike .cache/media/ball-720p.h264 \| diff - .cache/ref.txt` |
| `migraphx-devptr/` | MIGraphX on caller-owned device buffers: `run_async` stream ordering, exact partial batches, `.mxr` cache, FP16 | `spikes/migraphx-devptr/fetch-model.sh && make -C spikes/migraphx-devptr && spikes/migraphx-devptr/mgx_spike .cache/models/mobilenetv2-12.onnx` |

Known shortcuts. Don't lift this code unchanged into elements:
- even frame sizes only
- resolution changes are ignored
- no display-area crop offset is applied
- the MIGraphX dtype guard only checks the first parameter
