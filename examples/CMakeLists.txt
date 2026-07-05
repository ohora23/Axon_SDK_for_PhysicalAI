# Examples.
#   spike_poc — week 1-2 raw C PoC (V4L2 + SCM_RIGHTS)
#   demo      — closed-loop demo + measurement harness on the public dczc API

# Conditional add_subdirectory so the tree configures cleanly even before
# the spike PoC PR has landed.
if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/spike_poc)
    add_subdirectory(spike_poc)
endif()

if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/demo)
    add_subdirectory(demo)
endif()

if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/depth_filter)
    add_subdirectory(depth_filter)
endif()
