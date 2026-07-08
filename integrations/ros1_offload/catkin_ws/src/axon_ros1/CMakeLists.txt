cmake_minimum_required(VERSION 3.0.2)
project(axon_ros1)

# axon uses C++20. Ubuntu 20.04's default gcc-9 is partial; the Dockerfile sets
# CXX=g++-10.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(catkin REQUIRED COMPONENTS
  roscpp std_msgs sensor_msgs image_transport pluginlib message_generation)

add_message_files(FILES TensorDescriptor.msg AxonImage.msg)
generate_messages(DEPENDENCIES std_msgs)

catkin_package(
  INCLUDE_DIRS include
  CATKIN_DEPENDS roscpp std_msgs sensor_msgs image_transport pluginlib message_runtime)

# Prebuilt axon (headers + static lib) installed by the Dockerfile under /opt/axon.
set(AXON_ROOT "/opt/axon" CACHE PATH "axon source/build root")
set(AXON_LIB "${AXON_ROOT}/build/libaxon.a" CACHE FILEPATH "libaxon static lib")

# include/ holds the reusable, ROS-agnostic axon_bridge.h shared by nodes + plugin.
include_directories(include ${catkin_INCLUDE_DIRS} ${AXON_ROOT}/include)

# --- M1: descriptor-topic offload nodes ---
foreach(node producer_node consumer_node)
  add_executable(${node} src/${node}.cpp)
  add_dependencies(${node} ${${PROJECT_NAME}_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})
  target_link_libraries(${node} ${catkin_LIBRARIES} ${AXON_LIB} rt pthread)
endforeach()

# --- M2: "axon" image_transport plugin + its test nodes ---
add_library(axon_transport_plugins
  src/axon_image_publisher.cpp
  src/axon_image_subscriber.cpp)
add_dependencies(axon_transport_plugins
  ${${PROJECT_NAME}_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})
target_link_libraries(axon_transport_plugins ${catkin_LIBRARIES} ${AXON_LIB} rt pthread)

foreach(node image_pub_node image_sub_node)
  add_executable(${node} src/${node}.cpp)
  add_dependencies(${node} ${${PROJECT_NAME}_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})
  target_link_libraries(${node} ${catkin_LIBRARIES} ${AXON_LIB} rt pthread)
endforeach()

# Make the plugin discoverable by pluginlib at runtime.
install(TARGETS axon_transport_plugins
  LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION})
install(FILES axon_transport_plugins.xml
  DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION})
install(DIRECTORY include/${PROJECT_NAME}/
  DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION})
