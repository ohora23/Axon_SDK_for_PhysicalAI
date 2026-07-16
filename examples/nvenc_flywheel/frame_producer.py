# SPDX-License-Identifier: Apache-2.0
# axon NVENC data-flywheel demo — frame producer.
#
# Stands in for a camera / render source: it generates RGBA frames on the GPU
# (a moving gradient so the recorded video shows motion) and writes each one
# STRAIGHT into an axon Accelerator (CUDA VMM) pool buffer, then publishes only
# the descriptor. The pixels never leave the GPU — the recorder (nvenc_recorder.py)
# hardware-encodes them directly from this shared buffer.
#
# Publishes frames 1..FRAMES, then holds (re-publishing the last frame) so the
# pool stays alive while the recorder drains. Killed by the runner when done.

import os
import time

import torch

import axon

SERVICE = "flywheel/frames"
W = int(os.environ.get("VLA_W", "640"))
H = int(os.environ.get("VLA_H", "480"))
FRAMES = int(os.environ.get("VLA_FRAMES", "60"))


def make_frame(grid_y, grid_x, i: int) -> torch.Tensor:
    # A moving gradient. Channel order is ABGR (what the NVENC encoder is told to
    # expect); the exact mapping is cosmetic for a synthetic source.
    r = (grid_x + i * 4) % 256
    g = (grid_y + i * 2) % 256
    b = torch.full_like(r, (i * 8) % 256)
    a = torch.full_like(r, 255)
    return torch.stack([a, b, g, r], dim=-1).to(torch.uint8).contiguous()


def main() -> int:
    dev = "cuda"
    grid_y, grid_x = torch.meshgrid(
        torch.arange(H, device=dev), torch.arange(W, device=dev), indexing="ij")

    shape = [H, W, 4]
    nbytes = H * W * 4
    pool = axon.TensorPool.create(
        n_buffers=6, buffer_size=nbytes, backend=axon.PoolBackend.Accelerator)
    pub = axon.TensorPublisher.create(SERVICE, pool)
    print(f"[producer] {W}x{H} RGBA frames, {nbytes} B/frame, "
          f"publishing {FRAMES} then holding", flush=True)
    pub.handshake_pool()

    s = 0
    while True:
        s += 1
        idx = min(s, FRAMES)                      # after FRAMES, keep last frame
        frame = make_frame(grid_y, grid_x, idx)
        a = pub.acquire()
        dst = torch.as_tensor(
            pool.device_array(a.buffer_index, shape, axon.DType.U8), device=dev)
        dst.copy_(frame)
        torch.cuda.synchronize()
        pub.publish_device(a, shape, axon.DType.U8)
        if s == FRAMES:
            print(f"[producer] published all {FRAMES} frames; holding", flush=True)
        time.sleep(0.02)


if __name__ == "__main__":
    raise SystemExit(main())
