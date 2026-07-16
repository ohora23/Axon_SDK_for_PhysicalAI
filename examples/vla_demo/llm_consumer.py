# SPDX-License-Identifier: Apache-2.0
# axon VLA demo — LLM consumer.
#
# Receives a vision embedding from the vision producer through axon WITHOUT any
# host copy or serialization: latest_view() surfaces the producer's GPU buffer,
# which is aliased zero-copy as a torch CUDA tensor via __cuda_array_interface__.
# The embedding is projected to the LLM hidden size and fed to a small causal LM
# (GPT-2) as inputs_embeds — a VLM-style vision->LLM prefill handoff.
#
# Processes the first valid frame, prints the result, and exits.

import time

import torch
import torch.nn as nn
from transformers import AutoModelForCausalLM

import axon

SERVICE = "vla/vision_embed"
LLM = "gpt2"
MIN_SEQNO = 3  # let a few frames flow so we are past handshake/steady state


def to_torch(view, dev):
    # `view` (axon.TensorView) exposes __cuda_array_interface__ v3 over the
    # consumer-imported device buffer; torch aliases it with no copy.
    return torch.as_tensor(view, device=dev)


def main() -> int:
    dev = "cuda"
    llm = AutoModelForCausalLM.from_pretrained(LLM).to(dev).eval()
    hidden_llm = llm.config.n_embd  # 768 for gpt2

    sub = axon.TensorSubscriber.create(SERVICE)
    if sub.wait_handshake(30000) != 0:
        print("[consumer] handshake failed", flush=True)
        return 11
    print(f"[consumer] {LLM}: hidden={hidden_llm}; attached to '{SERVICE}'",
          flush=True)

    proj = None
    for _ in range(30000):
        v = sub.latest_view()
        if v is not None and v.seqno >= MIN_SEQNO:
            if v.device_ptr == 0:
                print("[consumer] frame is not device-backed", flush=True)
                return 12
            emb = to_torch(v, dev)                    # [1, S, H], zero-copy alias
            shape = tuple(emb.shape)

            # Integrity: the producer stamped the frame number into element 0.
            stamped = int(emb.view(-1)[0].item())
            if stamped != v.seqno:
                print(f"[consumer] integrity FAIL: stamp={stamped} seqno={v.seqno}",
                      flush=True)
                return 13

            s_len, hidden_v = shape[1], shape[2]
            if proj is None:
                torch.manual_seed(0)
                proj = nn.Linear(hidden_v, hidden_llm).to(dev).eval()
            with torch.no_grad():
                inp = proj(emb.float())               # [1, S, 768]
                out = llm(inputs_embeds=inp)          # prefill
                next_id = int(out.logits[0, -1].argmax())

            gib = emb.data_ptr()
            print(f"[consumer] frame seqno={v.seqno}: received {shape} embedding "
                  f"zero-copy (device_ptr=0x{v.device_ptr:x}, torch data_ptr="
                  f"0x{gib:x}, staleness={v.staleness_ns/1e6:.2f} ms)", flush=True)
            print(f"[consumer] integrity OK (stamp==seqno); vision->LLM prefill on "
                  f"{s_len} tokens produced next-token id={next_id}", flush=True)
            print("[consumer] SUCCESS: GPU embedding crossed the process boundary "
                  "with no host copy.", flush=True)
            return 0
        time.sleep(0.001)

    print("[consumer] never saw a valid frame", flush=True)
    return 14


if __name__ == "__main__":
    raise SystemExit(main())
