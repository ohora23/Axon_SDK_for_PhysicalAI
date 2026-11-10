enable_language(CUDA)
find_package(CUDAToolkit REQUIRED)
find_library(NVINFER nvinfer REQUIRED)

# The kernel is its own static library: linking axon (public cxx_std_20) into a target with CUDA
# sources would ask nvcc (CUDA 11.4 on JetPack 5) for a C++20 dialect it does not have.
add_library(axon_e2e_preprocess STATIC preprocess.cu)
set_target_properties(axon_e2e_preprocess PROPERTIES CUDA_ARCHITECTURES "72;87" CUDA_STANDARD 17)   # Xavier, Orin
target_link_libraries(axon_e2e_preprocess PUBLIC CUDA::cudart)

add_executable(axon_jetson_e2e jetson_e2e.cpp)
target_link_libraries(axon_jetson_e2e PRIVATE axon axon_e2e_preprocess CUDA::cuda_driver ${NVINFER})
