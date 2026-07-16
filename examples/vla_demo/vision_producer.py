# SPDX-License-Identifier: Apache-2.0
# axon VLA demo — vision producer.
#
# A small vision encoder (DeiT-tiny) runs on the GPU and writes each frame's
# embedding STRAIGHT into an axon Accelerator (CUDA VMM) pool buffer, then
# publishes only the descriptor. The embedding never leaves the GPU and is never
# serialized — a separate LLM process reads it zero-copy (see llm_consumer.py).
#
# Publishes continuously until killed, so the pool (and the latest buffer) stays
# alive while the consumer reads. The runner kills this once the consumer is done.

import time

import torch
from transformers import AutoModel

import axon

SERVICE = "vla/vision_embed"
ENCODER = "facebook/deit-tiny-patch16-224"


def main() -> int:
    dev = "cuda"
    torch.manual_seed(0)
    enc = AutoModel.from_pretrained(ENCODER).to(dev).eval()
    hidden = enc.config.hidden_size  # 192 for deit-tiny

    # One warm-up forward to learn the token-sequence length S.
    with torch.no_grad():
        px = torch.rand(1, 3, 224, 224, device=dev)
        seq = enc(pixel_values=px).last_hidden_state.shape[1]  # e.g. 197

    shape = [1, seq, hidden]
    nbytes = seq * hidden * 4  # float32
    pool = axon.TensorPool.create(
        n_buffers=4, buffer_size=nbytes, backend=axon.PoolBackend.Accelerator)
    pub = axon.TensorPublisher.create(SERVICE, pool)
    print(f"[producer] {ENCODER}: hidden={hidden} seq={seq} "
          f"embedding={shape} bytes={nbytes}", flush=True)
    pub.handshake_pool()

    s = 0
    while True:
        s += 1
        with torch.no_grad():
            px = torch.rand(1, 3, 224, 224, device=dev)
            emb = enc(pixel_values=px).last_hidden_state.contiguous()  # [1,S,H] f32

        a = pub.acquire()
        # Zero-copy alias of the pool buffer as a torch tensor, write the
        # embedding into it (a local device->device copy), then stamp the frame
        # number into element 0 so the consumer can verify the exact buffer.
        dst = torch.as_tensor(
            pool.device_array(a.buffer_index, shape, axon.DType.F32), device=dev)
        dst.copy_(emb)
        dst.view(-1)[0] = float(s)
        torch.cuda.synchronize()
        pub.publish_device(a, shape, axon.DType.F32)

        if s % 20 == 0:
            print(f"[producer] published {s} embeddings", flush=True)
        time.sleep(0.02)


if __name__ == "__main__":
    raise SystemExit(main())
