# dczc Python bindings

pybind11 bindings for the `dczc` C++ core (design doc §7.2, Phase 1). NumPy-native:
producers publish a NumPy array; consumers receive a **zero-copy, read-only NumPy
view backed directly by the dma-buf** — no copy on the read path.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDCZC_BUILD_PYTHON=ON
cmake --build build -j
export PYTHONPATH=$PWD/build/python   # where dczc.*.so lands
```

Requires `pybind11-dev` and Python 3 dev headers (`Development.Module`).

## Usage

```python
import dczc, numpy as np

# Producer
pool = dczc.TensorPool.create(n_buffers=8, buffer_size=256*1024,
                              backend=dczc.PoolBackend.Custom)
pub = dczc.TensorPublisher.create("camera/inference_out", pool)
pub.handshake_pool()
pub.publish(np.zeros((1, 3, 224, 224), np.uint8), dczc.DType.U8)

# Consumer
sub = dczc.TensorSubscriber.create("camera/inference_out")
sub.wait_handshake()
sub.set_fallback_policy(dczc.FallbackPolicy.LastKnownGood)
view = sub.latest_view()          # None on fallback, else a TensorView
if view is not None:
    arr = view.data               # NumPy array aliasing the dma-buf (read-only)
    print(view.seqno, view.staleness_ns, view.shape, arr.dtype)
```

The producer and consumer are normally separate processes (see
`tests/test_dczc.py`, which forks them). `view.data` stays valid as long as the
subscriber object is alive — the NumPy array holds the subscriber as its base.

## Test

```bash
(cd build && ctest -R python_binding --output-on-failure)
```
