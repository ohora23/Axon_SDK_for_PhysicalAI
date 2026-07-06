# axon closed-loop demo + measurement harness

A runnable demonstration of the `axon` public C++ API — no camera, no
accelerator. It streams tensors from a producer process to an RT-consumer
process over the real FD sidecar + seqlock metadata slot + dma-buf pool, then
reports the metrics the design doc asks for.

## Build & run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/examples/demo/axon_demo --frames 300 --rate-hz 200 --buffers 8
```

Options: `--frames N`, `--rate-hz R`, `--buffers B`, `--quiet`.

## What it measures

| Report line | Design doc | Meaning |
|---|---|---|
| end-to-end staleness | §5 (bounded staleness) | producer publish → RT consumer read, per frame; min/mean/p50/p90/p99/max |
| seqlock retries | §8.1 | reader retry distribution — ~0 in steady state |
| fallback invocations | §3.5 | times the LastKnownGood policy fired |
| payload errors | §1.2 | must be 0 — the seqno stamped into the dma-buf is read back uncopied |

The staleness tail tracks the consumer's polling period: at `--rate-hz R` the RT
loop ticks every `1/R` s, so a frame published just after a tick waits up to one
tick — the bound is `transport + one tick`, exactly the §4.3 model. The p50 shows
the sub-millisecond transport cost on its own.

## Topology

```
parent (producer)                child (RT consumer)
  TensorPool(Custom)               TensorSubscriber
  TensorPublisher                    wait_handshake()  ← FD sidecar
    handshake_pool()  ──────────▶    latest_view()     ← seqlock slot
    acquire → fill → publish ──▶     (record staleness, verify payload)
```

This is the seed of the week 9-12 closed-loop benchmark; the ROS2 baseline
comparison (§8.2) will run the same measurement against an equivalent ROS2
pipeline.
