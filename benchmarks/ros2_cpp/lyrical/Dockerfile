# ROS 2 Lyrical + the official rosidl::Buffer CUDA backend (ros2/rosidl_buffer_backends),
# for the GPU -> GPU comparison in ../README.md. Run with the host's NVIDIA driver:
#   docker build -t axon-bench-lyrical -f benchmarks/ros2_cpp/lyrical/Dockerfile benchmarks/ros2_cpp
#   docker run --rm --gpus all --ipc=host --pid=host axon-bench-lyrical
FROM ros:lyrical-ros-base

# The backend .deb depends on libnvidia-compute-595 (the user-space driver). Installing
# Ubuntu's copy inside the container could mismatch the host kernel driver, so satisfy
# the dependency with an empty package and let the NVIDIA runtime mount the host's
# libcuda instead.
RUN apt-get update && apt-get install -y --no-install-recommends equivs build-essential cmake \
 && printf 'Section: misc\nPriority: optional\nStandards-Version: 3.9.2\nPackage: libnvidia-compute-595\nVersion: 595.99.02-1\nDescription: placeholder; the NVIDIA container runtime mounts the host driver\n' > /tmp/nv.ctl \
 && cd /tmp && equivs-build nv.ctl && dpkg -i /tmp/libnvidia-compute-595_*.deb \
 && apt-get install -y --no-install-recommends ros-lyrical-cuda-buffer-backend ros-lyrical-cuda-buffer \
      ros-lyrical-sensor-msgs nvidia-cuda-dev \
 && rm -rf /var/lib/apt/lists/*

COPY lyrical/src /ws/src/axon_bench_lyrical
COPY src/ipc_event_probe.cpp /ws/ipc_event_probe.cpp
RUN . /opt/ros/lyrical/setup.sh && cd /ws && colcon build --cmake-args -DCMAKE_BUILD_TYPE=Release \
 && g++ -O2 ipc_event_probe.cpp -o ipc_event_probe -lcudart

COPY fastdds_zero_copy.xml /ws/fastdds_zero_copy.xml
COPY lyrical/run.sh /ws/run.sh
CMD ["/ws/run.sh"]
