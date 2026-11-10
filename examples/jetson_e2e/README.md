# Jetson camera → TensorRT inference, end to end

A UVC camera feeds a ResNet-18 (TensorRT, FP16) in another process, two ways:

- **zerocopy**: the driver writes each frame into an axon `NvBufSurface` pool slot (as in
  [`jetson_camera`](../jetson_camera/); no user-space copy); the consumer runs its
  pre-processing kernel straight on the frame's CUDA pointer.
- **copy**: the usual pipeline. V4L2 `MMAP` buffers are `memcpy`'d into a host pool, and
  the consumer copies the frame to the GPU (pageable `cudaMemcpyAsync` H2D) before the kernel.

Set `E2E_WARM_GPU=1` to wake the GPU before each frame is taken: at 30 fps the GPU idles between
frames, and otherwise the first timed GPU step also pays for waking it up.

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

`run_e2e.sh` alternates the two modes, consumer on CPU 4, producer on CPUs 0-3; results land in
`instrumentation/jetson/e2e/results/<tag>/` with an `env.txt` (GPU clocks, warm-up setting). By hand:

```bash
./build/examples/jetson_e2e/axon_jetson_e2e producer zerocopy /dev/video0            # terminal A
./build/examples/jetson_e2e/axon_jetson_e2e consumer zerocopy resnet18_fp16.engine 600 out.csv   # B
```

Results on AGX Xavier with a Logitech Webcam Pro 9000 (640×480 YUYV, 30 fps, USB 2.0) are in
[docs/hardware-verification.md §6](../../docs/hardware-verification.md).
