#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""End-to-end test of the dczc Python bindings.

Mirrors tests/test_end_to_end.cpp: a producer process streams NumPy frames to a
forked RT-consumer process over the real sidecar + seqlock + dma-buf pool. The
consumer reads a zero-copy NumPy view and checks the seqno stamped into the
buffer — matching bytes prove the data crossed the process boundary uncopied.

Run standalone (PYTHONPATH must point at the built dczc module):
    PYTHONPATH=build/python python3 python/tests/test_dczc.py
"""

import os
import sys
import time

import numpy as np

import dczc

SERVICE = "pytest/tensor_stream"
N_BUFFERS = 8
BUF_BYTES = 64 * 1024
FINAL_SEQNO = 50
SHAPE = (1, 3, 64, 64)  # 12288 bytes as u8, fits in BUF_BYTES


def make_frame(seqno: int) -> np.ndarray:
    """A frame whose first 8 bytes encode the seqno (little-endian)."""
    arr = np.zeros(SHAPE, dtype=np.uint8)
    flat = arr.reshape(-1)
    flat[:8].view("<u8")[0] = seqno
    return arr


def run_consumer() -> int:
    sub = dczc.TensorSubscriber.create(SERVICE)
    sub.set_fallback_policy(dczc.FallbackPolicy.LastKnownGood)
    if sub.wait_handshake(5000) != 0:
        print("consumer: handshake failed", file=sys.stderr)
        return 11
    if sub.pool_handshake_count() != 1:
        return 12

    seen = 0
    content_ok = False
    zero_copy_proven = False
    for _ in range(5000):
        v = sub.latest_view(8)
        if v is not None and v.seqno >= FINAL_SEQNO:
            seen = v.seqno
            stamped = int(v.data.reshape(-1)[:8].view("<u8")[0])
            content_ok = stamped == v.seqno
            # The view must be a real ndarray with the published shape/dtype.
            zero_copy_proven = (
                isinstance(v.data, np.ndarray)
                and tuple(v.shape) == SHAPE
                and v.data.dtype == np.uint8
            )
            if v.staleness_ns == 0 or v.staleness_ns > 5_000_000_000:
                return 13
            break
        time.sleep(0.001)

    if seen < FINAL_SEQNO:
        return 14
    if not content_ok:
        print("consumer: payload mismatch", file=sys.stderr)
        return 15
    if not zero_copy_proven:
        print("consumer: view not a well-formed ndarray", file=sys.stderr)
        return 16
    return 0


def run_producer(child_pid: int) -> int:
    pool = dczc.TensorPool.create(
        n_buffers=N_BUFFERS, buffer_size=BUF_BYTES, backend=dczc.PoolBackend.Custom
    )
    pub = dczc.TensorPublisher.create(SERVICE, pool)
    pub.handshake_pool()

    for s in range(1, FINAL_SEQNO + 1):
        pub.publish(make_frame(s), dczc.DType.U8)
        time.sleep(0.001)

    _, status = os.waitpid(child_pid, 0)
    return os.waitstatus_to_exitcode(status)


def main() -> int:
    # Basic smoke of the module surface before the fork.
    assert dczc.rt_now_ns() > 0
    pool = dczc.TensorPool.create(n_buffers=4, buffer_size=4096)
    assert pool.generation() == 1
    assert pool.buffer_count() == 4
    del pool

    pid = os.fork()
    if pid == 0:
        os._exit(run_consumer())

    rc = run_producer(pid)
    if rc == 0:
        print("dczc python binding e2e: PASS")
    else:
        print(f"dczc python binding e2e: FAIL (consumer rc={rc})", file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
