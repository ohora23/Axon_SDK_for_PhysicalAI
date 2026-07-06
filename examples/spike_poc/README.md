# Week 1-2 Spike PoC

> design doc §6.4 / diagrams §12 (decision tree)

## Validation Checklist

- [x] V4L2 capture → `VIDIOC_EXPBUF` exports a dma-buf FD
- [x] Listen on a Unix domain socket
- [x] `SCM_RIGHTS` delivers the dma-buf FD across processes
- [x] mmap the received FD as a host view (where the V4L2 driver allows it)
- [x] Publish metadata + observe hash changes to sanity-check zero-copy
- [ ] eBPF check that `copy_to_user` / `copy_from_user` stays at 0 (external script)
- [ ] Accelerator import (AMD XDNA / NVIDIA Jetson — once the board is chosen)
- [ ] 1kHz cyclictest baseline

## Build

```bash
cmake -S . -B ../../build/spike -DCMAKE_BUILD_TYPE=Release
cmake --build ../../build/spike -j
```

From the repo root:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAXON_BUILD_EXAMPLES=ON
cmake --build build -j
```

## Run

### Prerequisites
A USB UVC camera (or any V4L2-compatible camera) attached at `/dev/video0`.

```bash
v4l2-ctl --list-devices                    # list devices
v4l2-ctl -d /dev/video0 --list-formats-ext # list supported formats
```

### Run (two separate terminals)

**Terminal A — producer:**
```bash
./build/spike/axon_spike_producer /dev/video0
```
Sample output:
```
V4L2 format: 640x480 (614400 bytes/frame)
✓ buffer 0 → dma-buf FD 5
✓ buffer 1 → dma-buf FD 6
...
✓ listening on /tmp/axon_spike.sock — waiting for consumer
```

**Terminal B — consumer:**
```bash
./build/spike/axon_spike_consumer
```
Sample output:
```
✓ connected to producer
✓ handshake: pool_gen=1 n=4 size=614400 — received 4 FDs
  buffer 0: FD=4 → view 0x7f...
  ...
[consumer] seq=30 buf=2 staleness=0.412ms hash=0x...
[consumer] seq=60 buf=0 staleness=0.398ms hash=0x...
```

## What Counts as Spike Success

1. **Producer prints N dma-buf FDs** — `VIDIOC_EXPBUF` works.
2. **Consumer receives N FDs** + the hash changes every 30 frames — captured pixels reach the consumer view via zero-copy.
3. **Staleness < 1ms** between capture and publish — the sidecar + metadata publish path is fast enough.
4. **(Optional) eBPF verification** — the strong test:

```bash
# With bpftrace installed:
sudo bpftrace -e '
kprobe:copy_to_user, kprobe:copy_from_user / pid == @pid /
{ @copies[probe] = count(); }
END { print(@copies); }' &
# Then run the spike. @copies must not increase beyond V4L2 setup.
```

## When mmap Fails

Some V4L2 drivers refuse user-space mmap of dma-bufs. The right path is:
1. The accelerator driver does `dma_buf_attach` + `dma_buf_map_attachment` so the device IOMMU maps it.
2. From user space you only access through the accelerator SDK (CUDA, XDNA, ...).

This is **policy, not a bug**, and we still treat the spike as passing — full zero-copy verification happens in the accelerator-import phase (week 5-6).

## Next Steps

| Step | Action |
|---|---|
| 1 | Acquire a board (AMD AI Series or Jetson Orin) |
| 2 | Build and run this spike → validate the V4L2 + SCM_RIGHTS path |
| 3 | Smoke-test the accelerator import API (`amd_xdna_import_dma_buf` or `cuMemImportFromFd`) |
| 4 | eBPF verification of zero copies |
| 5 | 1kHz cyclictest baseline |
| 6 | Result → enter the formal design (week 4 implementation) or revisit the design doc |

## Known Limitations (this spike only)

- Metadata publish uses a plain stream socket instead of Iceoryx2 — replaced in the formal phase.
- The pool-reallocation (`pool_generation` bump) path is a stub — implemented later.
- Single consumer only. Fan-out lands in the formal phase.
- No sync_file fence — relies on the implicit fence attached at capture-complete.

## Cleanup

```bash
rm -f /tmp/axon_spike.sock
```
