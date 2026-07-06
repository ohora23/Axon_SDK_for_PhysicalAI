# axon ROS1 offload — metadata-only descriptor.
# The payload stays in a shared dma-buf delivered via the axon SCM_RIGHTS
# sidecar; only this fixed-size descriptor travels on the ROS1 topic, so
# rostopic/rosbag/watchdogs keep working while the tensor moves zero-copy.
uint64 bo_handle              # pool buffer index the payload lives in
uint64 seqno                  # monotonically increasing (seqlock/ABA + watchdog)
uint64 pool_generation        # bump => consumer must re-handshake the sidecar
uint64 capture_ts_ns          # sensor timestamp (here: producer stamp)
uint64 producer_publish_ts_ns # CLOCK_MONOTONIC_RAW just before publish
uint32[] shape                # tensor shape
uint8 rank
uint8 dtype                   # axon::DType enum value
uint64 offset                 # byte offset into the dma-buf
uint64 size                   # tensor size in bytes
