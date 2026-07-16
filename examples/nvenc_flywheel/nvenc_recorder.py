# SPDX-License-Identifier: Apache-2.0
# axon NVENC data-flywheel demo — recorder.
#
# The "data flywheel": capture the stream to disk for later training WITHOUT a
# CPU round-trip. Each frame is read from axon as a device pointer, aliased
# zero-copy as a torch CUDA tensor, and handed straight to the hardware encoder
# (NVENC via PyNvVideoCodec). The pixels go GPU buffer -> NVENC with no host copy
# and no re-upload; only the compressed bitstream touches the CPU, on its way to
# the file.
#
# Records the first CAPTURE distinct frames (by seqno) then flushes and exits.
# Under axon's latest-value-wins delivery a slow recorder may skip frames; the
# count of skipped frames is reported (raise the producer period or the pool
# depth if you need every frame).

import os
import time

import torch
import PyNvVideoCodec as nvc

import axon

SERVICE = "flywheel/frames"
CAPTURE = int(os.environ.get("VLA_FRAMES", "60"))
OUT = os.environ.get("VLA_OUT", "/tmp/axon_flywheel.h264")


def main() -> int:
    dev = "cuda"
    sub = axon.TensorSubscriber.create(SERVICE)
    if sub.wait_handshake(30000) != 0:
        print("[recorder] handshake failed", flush=True)
        return 11

    # Wait for the first device frame to learn the geometry.
    view = None
    for _ in range(30000):
        v = sub.latest_view()
        if v is not None and v.device_ptr != 0:
            view = v
            break
        time.sleep(0.001)
    if view is None:
        print("[recorder] no device frame arrived", flush=True)
        return 12

    h, w, _ = tuple(view.shape)
    enc = nvc.CreateEncoder(w, h, "ABGR", False, codec="h264")
    print(f"[recorder] {w}x{h} NVENC h264, recording {CAPTURE} frames straight "
          f"from axon device buffers", flush=True)

    data = bytearray()
    captured = 0
    last = -1
    dropped = 0
    t0 = time.time()
    while captured < CAPTURE and time.time() - t0 < 60:
        v = sub.latest_view()
        if v is None or v.device_ptr == 0 or v.seqno == last:
            time.sleep(0.001)
            continue
        if last >= 0 and v.seqno > last + 1:
            dropped += v.seqno - last - 1
        last = v.seqno
        frame = torch.as_tensor(v, device=dev)     # [H,W,4] zero-copy alias
        bitstream = enc.Encode(frame)              # NVENC reads the GPU buffer directly
        if bitstream:
            data += bytes(bitstream)
        captured += 1

    tail = enc.EndEncode()                         # flush buffered frames
    if tail:
        data += bytes(tail)

    with open(OUT, "wb") as f:
        f.write(data)
    if captured == 0 or len(data) == 0:
        print("[recorder] FAIL: nothing encoded", flush=True)
        return 13
    print(f"[recorder] encoded {captured} frames "
          f"({dropped} skipped by latest-value-wins) -> {len(data)} bytes at {OUT}",
          flush=True)

    # Self-check: the written stream must decode back to the frames we encoded.
    demux = nvc.CreateDemuxer(OUT)
    dec = nvc.CreateDecoder()
    decoded = sum(1 for pkt in demux for _ in dec.Decode(pkt))
    if decoded != captured:
        print(f"[recorder] verify FAIL: decoded {decoded} != encoded {captured}",
              flush=True)
        return 14
    print(f"[recorder] verify OK: {OUT} decodes to {decoded} frames", flush=True)
    print("[recorder] SUCCESS: pixels went axon GPU buffer -> NVENC with no host "
          "copy.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
