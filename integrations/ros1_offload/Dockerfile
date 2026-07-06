# axon ROS1 offload PoC. Build context = repo root.
#   docker build -f integrations/ros1_offload/Dockerfile -t axon-ros1 .
FROM ros:noetic-ros-base

# Ubuntu 20.04's apt cmake is 3.16; axon needs >= 3.22, so install a newer one
# via pip (lands in /usr/local/bin, ahead of /usr/bin on PATH).
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-10 build-essential python3-pip \
    && pip3 install --no-cache-dir "cmake>=3.22,<3.28" \
    && rm -rf /var/lib/apt/lists/*

# --- Build axon (library only) with g++-10 for C++20 ---
COPY CMakeLists.txt /opt/axon/CMakeLists.txt
COPY cmake          /opt/axon/cmake
COPY include        /opt/axon/include
COPY src            /opt/axon/src
RUN cmake -S /opt/axon -B /opt/axon/build \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-10 \
        -DAXON_BUILD_EXAMPLES=OFF -DAXON_BUILD_TESTS=OFF \
 && cmake --build /opt/axon/build -j

# --- Build the catkin workspace (descriptor msg + producer/consumer nodes) ---
COPY integrations/ros1_offload/catkin_ws /catkin_ws
RUN /bin/bash -c "source /opt/ros/noetic/setup.bash && cd /catkin_ws && \
    catkin_make -DCMAKE_CXX_COMPILER=g++-10"

COPY integrations/ros1_offload/demo.sh /demo.sh
RUN chmod +x /demo.sh
CMD ["/demo.sh"]
