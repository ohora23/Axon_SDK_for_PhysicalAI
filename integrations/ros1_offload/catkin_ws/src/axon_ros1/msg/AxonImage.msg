# axon zero-copy image_transport message.
#
# Carries the sensor_msgs/Image metadata (everything EXCEPT the bulk pixel data)
# plus a handle into the axon dma-buf pool. The pixels stay in the shared dma-buf
# delivered once by the axon SCM_RIGHTS sidecar; only this small descriptor
# travels on the ROS topic, so rostopic/rosbag/watchdogs keep working.
std_msgs/Header header
uint32 height
uint32 width
string encoding
uint8  is_bigendian
uint32 step
# --- axon pool handle (payload lives in the dma-buf, not in this message) ---
uint64 bo_handle          # pool buffer index the frame lives in
uint64 seqno              # monotonically increasing
uint32 pool_generation    # bump => subscriber must re-handshake the sidecar
uint64 offset             # byte offset into the dma-buf
uint64 size               # frame size in bytes (step * height)
