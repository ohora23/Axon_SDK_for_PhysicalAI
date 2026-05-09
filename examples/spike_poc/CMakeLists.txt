# Week 1-2 Spike PoC
# Validation items (design doc §6.4):
#   1. V4L2 capture → VIDIOC_EXPBUF exports a dma-buf FD
#   2. SCM_RIGHTS delivers the FD across processes
#   3. The received FD is mmap'd as a zero-copy view (verified via eBPF)
#   4. (Optional) Accelerator import — DCZC_ENABLE_AMD_XDNA / DCZC_ENABLE_JETSON

add_executable(dczc_spike_producer producer.c)
add_executable(dczc_spike_consumer consumer.c)

foreach(tgt dczc_spike_producer dczc_spike_consumer)
    target_link_libraries(${tgt} PRIVATE dczc)
    target_compile_options(${tgt} PRIVATE -D_GNU_SOURCE)
endforeach()

if(DCZC_ENABLE_AMD_XDNA)
    target_compile_definitions(dczc_spike_consumer PRIVATE DCZC_BACKEND_AMD_XDNA=1)
    # find_package(XDNA REQUIRED)  # to be wired in during the spike
endif()

if(DCZC_ENABLE_JETSON)
    target_compile_definitions(dczc_spike_consumer PRIVATE DCZC_BACKEND_JETSON=1)
    # find_package(CUDAToolkit REQUIRED)
    # target_link_libraries(dczc_spike_consumer PRIVATE CUDA::cudart)
endif()
