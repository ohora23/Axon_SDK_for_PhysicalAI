# NVENC data-flywheel demo — record from the shared GPU buffer

**English** | [한국어](README.ko.md)

A *data flywheel* is the loop where a robot's live sensor stream is recorded to
build the dataset that trains the next model. The expensive part is the
recording: naively you copy every frame off the GPU, across the CPU, and into an
encoder — a full host round-trip per frame.

This demo does it the axon way. Frames live in a shared **CUDA** buffer; the
recorder hands each one **directly to the hardware video encoder (NVENC)** with
no host copy and no re-upload. Only the compressed bitstream — orders of
magnitude smaller — ever touches the CPU, on its way to the file.

```
 frame_producer.py                          nvenc_recorder.py
 ┌────────────────────────┐                 ┌──────────────────────────────┐
 │ render RGBA on GPU      │                 │ torch.as_tensor(view)        │
 │ write into axon pool    │ axon descriptor │  (zero-copy CAI alias)       │
 │ buffer (device)         │ ──────────────► │ NVENC.Encode(frame)          │
 │ publish_device()        │  (pixels stay   │  GPU buffer → encoder,       │
 │                         │   on the GPU)   │  no host copy → .h264 file   │
 └────────────────────────┘                 └──────────────────────────────┘
```

## What it proves

- The recorder wraps `latest_view()` as a torch CUDA tensor via
  `__cuda_array_interface__` and passes it straight to `NVENC.Encode(...)`. The
  pixels are read by the encoder **out of the producer's shared buffer** — no
  intermediate host copy, no `cudaMemcpy` back to the GPU.
- The written `.h264` is decoded back and its frame count is checked against the
  number encoded — proving a real, valid video came out.
- Because axon is **latest-value-wins**, a recorder that falls behind skips
  frames rather than blocking the producer; the demo reports how many were
  skipped (`0` when the recorder keeps up). If you need every frame, raise the
  producer period or the pool depth (see the sizing note in
  [`docs/usage.md`](../../docs/usage.md)).

## Requirements

- Build the axon Python module with CUDA:
  `cmake -S . -B build-cuda -DAXON_BUILD_PYTHON=ON -DAXON_WITH_CUDA=ON && cmake --build build-cuda -j`
- A Python env with `torch` (CUDA build matching the GPU) and
  [`PyNvVideoCodec`](https://pypi.org/project/PyNvVideoCodec/)
  (`pip install PyNvVideoCodec`). On this machine: `/home/jkyoo/.venvs/axon-vla`.
- An NVIDIA GPU with NVENC (most GeForce/RTX and datacenter cards).

## Run

```bash
examples/nvenc_flywheel/run_demo.sh
```

Environment overrides:

```bash
VLA_W=1280 VLA_H=720 VLA_FRAMES=120 \
VLA_OUT=/tmp/clip.h264 \
VLA_PYTHON=/path/to/venv/bin/python \
AXON_SO_DIR=/path/to/build-cuda/python \
examples/nvenc_flywheel/run_demo.sh
```

Expected tail:

```
[recorder] encoded 60 frames (0 skipped by latest-value-wins) -> 14949 bytes at /tmp/axon_flywheel.h264
[recorder] verify OK: /tmp/axon_flywheel.h264 decodes to 60 frames
[recorder] SUCCESS: pixels went axon GPU buffer -> NVENC with no host copy.
```

The output is a raw H.264 elementary stream. Wrap it in a container to play it,
e.g. `ffmpeg -i /tmp/axon_flywheel.h264 -c copy clip.mp4`.

## Files

- `frame_producer.py` — synthesises RGBA frames on the GPU and publishes them
  into an axon `Accelerator` pool.
- `nvenc_recorder.py` — reads each frame zero-copy and NVENC-encodes it to a
  `.h264` file, then decode-verifies the result.
- `run_demo.sh` — launches both processes and reports the verdict.
