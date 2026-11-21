# Jetson camera → TensorRT inference, end to end

A UVC camera feeds a ResNet-18 (TensorRT, FP16) in another process, two ways:

- **zerocopy**: the driver writes each frame into an axon `NvBufSurface` pool slot (as in
  [`jetson_camera`](../jetson_camera/); no user-space copy); the consumer runs its
  pre-processing kernel straight on the frame's CUDA pointer.
- **copy**: the usual pipeline. V4L2 `MMAP` buffers are `memcpy`'d into a host pool, and
  the consumer copies the frame to the GPU (pageable `cudaMemcpyAsync` H2D) before the kernel.

Set `E2E_WARM_GPU=1` to wake the GPU before each frame is taken: at 30 fps the GPU idles between
frames, and otherwise the first timed GPU step also pays for waking it up. The warm-up runs
before the frame is read, so its time shows up in publish → received instead.

Both then convert YUYV to normalised RGB at 224×224 on the GPU (`preprocess.cu`) and run the
engine. Each frame logs the capture time (the V4L2 timestamp, moved from `CLOCK_MONOTONIC` to
`CLOCK_MONOTONIC_RAW`), publish, receive, data-on-GPU, pre-processed and inferred times.

## Build and run (on the Jetson)

```bash
cmake -S . -B build -DAXON_ENABLE_JETSON=ON -DAXON_WITH_CUDA=ON && cmake --build build -j
# a TensorRT engine, e.g. ResNet-18 from the ONNX model zoo:
/usr/src/tensorrt/bin/trtexec --onnx=resnet18-v1-7.onnx --fp16 --saveEngine=resnet18_fp16.engine
E2E_WARM_GPU=1 instrumentation/jetson/e2e/run_e2e.sh build resnet18_fp16.engine 600 2 my_run
```

Other cameras and formats: set `E2E_FORMAT` (`YUYV` or `NV12`), `E2E_WIDTH`, `E2E_HEIGHT` (default
640×480 YUYV) for both processes, and `E2E_DEVICE` for `run_e2e.sh` (default `/dev/video0`). NV12 goes
into a GRAY8 surface of W × 1.5H rows (Y plane, then the interleaved UV plane). An OAK-D in UVC mode, for
example (1280×800 NV12 over USB 3):

```bash
E2E_FORMAT=NV12 E2E_WIDTH=1280 E2E_HEIGHT=800 E2E_DEVICE=/dev/video2 E2E_WARM_GPU=1 \
  instrumentation/jetson/e2e/run_e2e.sh build resnet18_fp16.engine 600 2 oak_nv12
```

The zero-copy path needs the camera to accept the surface pitch as `bytesperline`. NvBufSurface pads the
pitch when the row is not a multiple of its alignment (observed on Xavier: a 320 B row of 160 px YUYV gets
a 512 B pitch; 1280 B rows are not padded), and uvcvideo cameras usually refuse a padded `bytesperline`
(`camera can't do ... at pitch`). Pick a width whose row bytes are a multiple of 256, or use copy mode.
The kernels are checked against a CPU reference by `test_preprocess.cu` (build line at its top).

`run_e2e.sh` alternates the two modes, consumer on CPU 4, producer on CPUs 0-3; results land in
`instrumentation/jetson/e2e/results/<tag>/` with an `env.txt` (GPU clocks, warm-up setting). By hand:

```bash
./build/examples/jetson_e2e/axon_jetson_e2e producer zerocopy /dev/video0            # terminal A
./build/examples/jetson_e2e/axon_jetson_e2e consumer zerocopy resnet18_fp16.engine 600 out.csv   # B
```

Results on AGX Xavier with a Logitech Webcam Pro 9000 (640×480 YUYV, 30 fps, USB 2.0) are in
[docs/hardware-verification.md §6](../../docs/hardware-verification.md).
