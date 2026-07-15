# SPDX-License-Identifier: Apache-2.0
# A-2: consumer receives an Accelerator (GPU) frame through the library and
# surfaces it as a CUDA Array Interface.
#
# A forked producer publishes device frames (Accelerator pool + publish_device);
# the consumer reads latest_view() and checks view.device_ptr + the consumer
# __cuda_array_interface__ dict are well-formed — framework-free (no CuPy needed;
# content is proven separately by the C++ test test_accel_e2e). SKIPs cleanly
# (exit 0) when built without AXON_WITH_CUDA or no CUDA device is present.
#
# CUDA + fork() is unsafe once a context exists, so fork FIRST and let each side
# init CUDA on its own.

import os
import sys
import time

import axon

N = 1024
SERVICE = "py/accel_stream"
FINAL = 30


def run_consumer() -> int:
    try:
        sub = axon.TensorSubscriber.create(SERVICE)
    except Exception:
        return 10
    if sub.wait_handshake(5000) != 0:
        return 11
    for _ in range(5000):
        v = sub.latest_view()
        if v is not None and v.seqno >= FINAL:
            if v.device_ptr == 0:
                return 12
            cai = v.__cuda_array_interface__
            if cai is None:
                return 13
            if cai["version"] != 3:
                return 14
            if tuple(cai["shape"]) != (N,):
                return 15
            if cai["typestr"] != "<f4":
                return 16
            ptr, read_only = cai["data"]
            if ptr != v.device_ptr or read_only is not True:
                return 17
            if cai["strides"] is not None:
                return 18
            return 0
        time.sleep(0.001)
    return 19


def main() -> int:
    pid = os.fork()
    if pid == 0:
        os._exit(run_consumer())

    # Parent = producer. Pool creation inits CUDA on this side (after fork).
    try:
        pool = axon.TensorPool.create(
            n_buffers=6, buffer_size=N * 4,
            backend=axon.PoolBackend.Accelerator)
    except Exception as e:
        print("SKIP: Accelerator pool unavailable:", e)
        os.waitpid(pid, 0)   # child times out its handshake and exits
        return 0

    pub = axon.TensorPublisher.create(SERVICE, pool)
    pub.handshake_pool()
    for _ in range(1, FINAL + 1):
        a = pub.acquire()
        # A real producer writes into pool.device_array(a.buffer_index) here
        # (via CuPy/PyTorch); this test verifies the descriptor + consumer CAI
        # plumbing, so it publishes the acquired buffer as-is.
        pub.publish_device(a, [N], axon.DType.F32)
        time.sleep(0.002)

    _, status = os.waitpid(pid, 0)
    if not (os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0):
        code = os.WEXITSTATUS(status) if os.WIFEXITED(status) else -1
        print("FAIL: consumer exit code =", code)
        return 1
    print("PASS: consumer device frame + __cuda_array_interface__ verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
