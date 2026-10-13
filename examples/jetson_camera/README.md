# Jetson camera → CUDA zero-copy demo

A UVC camera captures **straight into** an axon `PoolBackend::NvBufSurface` pool; a
second process reads each frame both as a host view (`TensorView::data`) and as a CUDA
device pointer (`TensorView::device_ptr`). Nothing is copied on the way — the consumer's
check copies the device view back once every 30 frames only to prove it holds the same
bytes as the host view.

```
UVC camera ──DMA──▶ NvBufSurface (dma-buf) ──SCM_RIGHTS + import params──▶ consumer
                         ▲                                                 ├─ data       (mmap)
                         └── producer publishes only the descriptor ───▶  └─ device_ptr (EGL → CUDA)
```

## Build (on the Jetson)

```bash
cmake -S . -B build -DAXON_ENABLE_JETSON=ON -DAXON_WITH_CUDA=ON
cmake --build build -j
```

JetPack 5 ships cmake 3.16; axon needs ≥ 3.22 (a Kitware release tarball works without root).

## Run

```bash
./build/examples/jetson_camera/axon_jetson_camera producer /dev/video0   # terminal A
./build/examples/jetson_camera/axon_jetson_camera consumer               # terminal B
```

Measured on AGX Xavier (JetPack 5.1.6) + Logitech Webcam Pro 9000, 640×480 YUYV:

```
consumer: seq=30 staleness(avg)=440 us device_ptr=0x20393a000 host==device: yes
...
consumer: 120 frames, host==device on 4/4 checks
```

The staleness includes the consumer's 1 ms poll sleep.
