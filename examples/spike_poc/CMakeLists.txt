# Week 1-2 Spike PoC
# Validation items (design doc §6.4):
#   1. V4L2 capture → VIDIOC_EXPBUF exports a dma-buf FD
#   2. SCM_RIGHTS delivers the FD across processes
#   3. The received FD is mmap'd as a zero-copy view (verified via eBPF)
#   4. (Optional) Accelerator import — AXON_ENABLE_AMD_XDNA / AXON_ENABLE_JETSON

add_executable(axon_spike_producer producer.c)
add_executable(axon_spike_consumer consumer.c)

foreach(tgt axon_spike_producer axon_spike_consumer)
    target_link_libraries(${tgt} PRIVATE axon)
    target_compile_options(${tgt} PRIVATE -D_GNU_SOURCE)
endforeach()

if(AXON_ENABLE_AMD_XDNA)
    target_compile_definitions(axon_spike_consumer PRIVATE AXON_BACKEND_AMD_XDNA=1)
    # find_package(XDNA REQUIRED)  # to be wired in during the spike
endif()

if(AXON_ENABLE_JETSON)
    target_compile_definitions(axon_spike_consumer PRIVATE AXON_BACKEND_JETSON=1)
    # find_package(CUDAToolkit REQUIRED)
    # target_link_libraries(axon_spike_consumer PRIVATE CUDA::cudart)
endif()
