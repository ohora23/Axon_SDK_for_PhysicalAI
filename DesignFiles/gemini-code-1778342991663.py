markdown_content = """# Detailed Design — Data-Centric Physical AI Robot Architecture (initial memo)

This document captured the initial direction for a **data-centric robot software architecture** designed to overcome the latency caused by ROS2 DDS serialization/deserialization and memory bouncing, and to meet the high-bandwidth / ultra-low-latency demands of Physical AI. The original architecture split into a unified-memory environment (UMA) and a discrete NUMA environment, with hard real-time scheduling on RT-Linux.

> Status: superseded by `data-centric-zero-copy-design-20260510.md` (APPROVED v2) and `detailed_design_doc.md`. Kept here for archival reference.

---

## 1. [A] Unified Memory Architecture (UMA: Apple Silicon, AMD AI Series CPU)

An environment where CPU, GPU, and NPU physically share the same system RAM. There is no PCIe bottleneck, and cache coherency plus pointer passing are the focus.

### 1.1 Data plane: Iceoryx2-based zero-copy tensor bus
- **Mechanism:** lock-free queue for metadata combined with a large shared-memory pool for payloads.
- **Data transfer:** the publisher writes directly into the subscriber's memory space. Tensor data lives in the payload pool; only a **memory descriptor** containing shape and a memory pointer flows through the message queue.
- **Performance:** O(1) regardless of payload size for large tensors.

### 1.2 Memory plane: Linux DMA-BUF sharing
- **Zero-copy implementation:** Linux `dma-buf` shares buffers across hardware drivers as file descriptors (FDs) without physical copies.
- **Operation:** a frame captured by the V4L2 driver is extracted via `dma_buf_export` and imported into the NPU driver via `dma_buf_import`.
- **Synchronization:** the `dma_resv` object controls when the CPU cache is flushed to prevent conflicts.

```c
// [Pseudocode] UMA accelerator memory mapping function
int map_tensor_uma(int dma_buf_fd, size_t size, void** cpu_ptr, void** npu_ptr) {
    // 1. Map into CPU user space (cache coherency must be maintained)
    *cpu_ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dma_buf_fd, 0);

    // 2. Import into NPU device memory (translates the physical address and
    //    registers it in the page table)
    *npu_ptr = amd_xdna_import_dma_buf(device_handle, dma_buf_fd, size);

    // 3. After flushing the CPU cache, trigger the NPU operation
    ioctl(dma_buf_fd, DMA_BUF_IOCTL_SYNC, &sync_start_flags);
    return 0;
}
"""
