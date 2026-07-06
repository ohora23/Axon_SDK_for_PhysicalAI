# Depth-filter demo: synthetic depth -> CPU hole-fill -> zero-copy publish,
# exercising the v2 imaging/depth descriptor metadata (row_pitch, depth_scale,
# units, invalid_value, intrinsics). No camera required.
add_executable(axon_depth_filter depth_filter_demo.cpp)
target_link_libraries(axon_depth_filter PRIVATE axon)

if(AXON_BUILD_TESTS)
    add_test(NAME depth_filter_smoke COMMAND axon_depth_filter)
    set_tests_properties(depth_filter_smoke PROPERTIES TIMEOUT 30)
endif()
