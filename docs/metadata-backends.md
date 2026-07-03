# Metadata-plane backends

The metadata plane (design doc §1.1) is the lock-free channel that carries the
fixed-size `TensorDescriptor` between producer and RT consumer. `dczc` exposes it
behind an abstract interface (`dczc::detail::MetadataChannel`) with two
interchangeable backends. This is the design-doc promise — swapping in Iceoryx2
"changes only this file" — made concrete.

| Backend | Source | Dependency | Default |
|---|---|---|---|
| **Seqlock** | `src/metadata_channel.cpp` | none (POSIX SHM) | yes (when Iceoryx2 not compiled) |
| **Iceoryx2** | `src/metadata_channel_iox2.cpp` | Iceoryx2 C++ bindings | yes (when compiled in) |

Both implement single-producer / multi-consumer **latest-value-wins** semantics —
exactly what the RT consumer's `latest_view()` needs.

## Selecting a backend

- **Compile time:** `-DDCZC_WITH_ICEORYX2=ON` compiles the Iceoryx2 backend in
  (otherwise only the seqlock backend exists).
- **Run time:** `DCZC_METADATA_BACKEND=seqlock|iceoryx2` (only meaningful when
  Iceoryx2 was compiled in). Default is `iceoryx2` when available. If Iceoryx2
  initialization fails, the factory falls back to the always-available seqlock
  backend.

## Building with Iceoryx2

Iceoryx2 is Rust-native with C++ bindings built via CMake. Build and install it
once (requires a Rust toolchain):

```bash
git clone --depth 1 --branch v0.9.2 https://github.com/eclipse-iceoryx/iceoryx2
cmake -S iceoryx2 -B iceoryx2/build -DBUILD_CXX=ON -DBUILD_EXAMPLES=OFF \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/iceoryx2
cmake --build iceoryx2/build --target install -j
```

Then build `dczc` against it:

```bash
cmake -S . -B build -DDCZC_WITH_ICEORYX2=ON -DCMAKE_PREFIX_PATH=/opt/iceoryx2
cmake --build build -j
(cd build && ctest --output-on-failure)
```

## Which to use

The **seqlock backend is the better default for the single-host, latest-value
pattern**: a read is an immediate load of the current slot, with no queue
draining. In a local A/B run (200 frames @ 200 Hz, 256 KiB) the seqlock backend
showed lower p50 end-to-end staleness than Iceoryx2, because Iceoryx2's queue +
`receive()` model adds buffering latency for this access pattern.

**Iceoryx2 earns its place** where its strengths matter: robust service discovery,
history/QoS, multi-process fan-out at scale, and cross-language (Rust/C/C++)
interop. The backend interface lets a deployment pick per workload without
touching `TensorPublisher` / `TensorSubscriber`.
