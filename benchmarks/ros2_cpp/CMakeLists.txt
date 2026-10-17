cmake_minimum_required(VERSION 3.22)
project(axon_bench)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(CUDAToolkit REQUIRED)   # bench.cpp uses the CUDA driver API

# Determine AXON_ROOT (two dirs up = repo root)
get_filename_component(_bench_dir "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
get_filename_component(_bench_parent "${_bench_dir}" DIRECTORY)
set(AXON_ROOT "${_bench_parent}" CACHE PATH "Axon SDK root directory")

# AXON_LIB: path to the prebuilt static library
set(AXON_LIB "${AXON_ROOT}/build-cuda/libaxon.a" CACHE PATH "Path to libaxon.a")

# --- Messages ---
rosidl_generate_interfaces(${PROJECT_NAME}
  msg/Payload147K.msg
  msg/Payload1M.msg
  msg/Payload4M.msg
)

# --- Benchmark executable ---
add_executable(bench src/bench.cpp)

target_include_directories(bench PRIVATE
  ${AXON_ROOT}/include
)

target_compile_features(bench PRIVATE cxx_std_20)
target_compile_definitions(bench PRIVATE AXON_WITH_CUDA=1)

ament_target_dependencies(bench
  rclcpp
  std_msgs
)

# Get the typesupport target for the generated messages
rosidl_get_typesupport_target(cpp_typesupport_target "${PROJECT_NAME}" "rosidl_typesupport_cpp")

target_link_libraries(bench ${cpp_typesupport_target} ${AXON_LIB} CUDA::cuda_driver rt pthread)

# Install
install(TARGETS bench
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
