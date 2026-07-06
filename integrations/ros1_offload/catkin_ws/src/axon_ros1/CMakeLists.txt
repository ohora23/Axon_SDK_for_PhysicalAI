cmake_minimum_required(VERSION 3.0.2)
project(axon_ros1)

# axon uses C++20. Ubuntu 20.04's default gcc-9 is partial; the Dockerfile sets
# CXX=g++-10.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(catkin REQUIRED COMPONENTS roscpp std_msgs message_generation)

add_message_files(FILES TensorDescriptor.msg)
generate_messages(DEPENDENCIES std_msgs)

catkin_package(CATKIN_DEPENDS roscpp std_msgs message_runtime)

# Prebuilt axon (headers + static lib) installed by the Dockerfile under /opt/axon.
set(AXON_ROOT "/opt/axon" CACHE PATH "axon source/build root")
set(AXON_LIB "${AXON_ROOT}/build/libaxon.a" CACHE FILEPATH "libaxon static lib")

include_directories(${catkin_INCLUDE_DIRS} ${AXON_ROOT}/include)

foreach(node producer_node consumer_node)
  add_executable(${node} src/${node}.cpp)
  add_dependencies(${node} ${${PROJECT_NAME}_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})
  target_link_libraries(${node} ${catkin_LIBRARIES} ${AXON_LIB} rt pthread)
endforeach()
