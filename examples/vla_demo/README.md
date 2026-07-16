# VLA demo — vision → LLM zero-copy handoff

A two-process demonstration of Direction A: a **vision encoder** hands its GPU
embedding to a **language model** in a *separate process* with **no host copy and
no serialization**. The embedding stays resident in a shared CUDA buffer; only a
small descriptor crosses the process boundary. This is the VLM inner loop axon is
built for.

```
 vision_producer.py                         llm_consumer.py
 ┌───────────────────────┐                  ┌────────────────────────────┐
 │ DeiT-tiny (GPU)       │                  │ GPT-2 (GPU)                │
 │  image → embedding    │                  │  inputs_embeds → prefill   │
 │  write into axon pool │  axon descriptor │  torch.as_tensor(view)     │
 │  buffer (device)      │ ───────────────► │  (zero-copy CAI alias)     │
 │  publish_device()     │  (payload stays  │  project → LLM → next tok  │
 └───────────────────────┘   on the GPU)    └────────────────────────────┘
```

The embedding is aliased on both sides through `__cuda_array_interface__`:
`pool.device_array(...)` on the producer and `view` (the `latest_view()` result)
on the consumer, both wrapped by `torch.as_tensor(...)` with no copy.

## What it proves

- The producer stamps each frame's number into element 0 of the embedding; the
  consumer reads it back and checks `stamp == view.seqno` — so the *exact* GPU
  buffer content crossed the boundary.
- `view.device_ptr` is a real device pointer and equals the torch tensor's
  `data_ptr()` — the LLM reads the producer's buffer directly.
- The LLM runs a real prefill on the received embedding (`inputs_embeds`).

## Requirements

- Build the axon Python module with CUDA:
  `cmake -S . -B build-cuda -DAXON_BUILD_PYTHON=ON -DAXON_WITH_CUDA=ON && cmake --build build-cuda -j`
- A Python env with `torch` (CUDA build matching the GPU) and `transformers`.
  On this machine: `/home/jkyoo/.venvs/axon-vla` (torch cu128 for the RTX 5080).

## Run

```bash
examples/vla_demo/run_demo.sh
```

Override the interpreter or module location if needed:

```bash
VLA_PYTHON=/path/to/venv/bin/python \
AXON_SO_DIR=/path/to/build-cuda/python \
examples/vla_demo/run_demo.sh
```

The runner starts the producer (publishes continuously), runs the consumer (which
processes one frame and exits `0` on success), then stops the producer.

## Models

- Vision: `facebook/deit-tiny-patch16-224` (~23 MB) — embedding `[1, 197, 192]`.
- LLM: `gpt2` (~500 MB) — hidden size 768; a random `Linear(192→768)` connector
  bridges the dimensions (this demo exercises the data path, not a trained VLM).

They download on first run via `transformers`.
