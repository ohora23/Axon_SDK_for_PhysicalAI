# R6 accelerator-pool demo — cross-process device zero-copy via the library API.
# Only configured when AXON_WITH_CUDA is on (the axon target then carries the
# CUDA VMM backend and the CUDA::cuda_driver link).

add_executable(axon_accelerator_pool_demo accelerator_pool_demo.cpp)
target_link_libraries(axon_accelerator_pool_demo PRIVATE axon CUDA::cuda_driver)

# Runs only where a CUDA device is present; harmless to register (skips if the
# GPU is busy/absent by returning non-zero, which surfaces as a test failure —
# so gate it behind an opt-in to keep `ctest` green on CI without a GPU).
if(AXON_TEST_CUDA)
    add_test(NAME accelerator_pool_smoke COMMAND axon_accelerator_pool_demo)
endif()
