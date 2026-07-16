cmake_minimum_required(VERSION 3.8)
project(axon_ros2)

# axon core is C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rosidl_default_generators REQUIRED)

# Metadata-only message (payload never rides DDS).
rosidl_generate_interfaces(${PROJECT_NAME} "msg/TensorDescriptor.msg")

# Prebuilt axon core (headers + static lib) from this repo. Build it first:
#   cmake -S <repo> -B <repo>/build && cmake --build <repo>/build -j
get_filename_component(AXON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(AXON_ROOT "${AXON_ROOT}" CACHE PATH "axon repo root")
set(AXON_LIB "${AXON_ROOT}/build/libaxon.a" CACHE FILEPATH "libaxon static lib")

# include/ holds the reusable, ROS-agnostic axon_bridge.h (shared with ROS1).
set(AXON_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${AXON_ROOT}/include)

# The generated C++ message typesupport, linked into both nodes.
rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} rosidl_typesupport_cpp)

foreach(node producer_node consumer_node)
  add_executable(${node} src/${node}.cpp)
  target_include_directories(${node} PRIVATE ${AXON_INCLUDES})
  ament_target_dependencies(${node} rclcpp)
  target_link_libraries(${node} "${cpp_typesupport_target}" ${AXON_LIB} rt pthread)
  install(TARGETS ${node} DESTINATION lib/${PROJECT_NAME})
endforeach()

ament_package()
