# Depth-filter demo: synthetic depth -> CPU hole-fill -> zero-copy publish,
# exercising the v2 imaging/depth descriptor metadata (row_pitch, depth_scale,
# units, invalid_value, intrinsics). No camera required.
add_executable(dczc_depth_filter depth_filter_demo.cpp)
target_link_libraries(dczc_depth_filter PRIVATE dczc)

if(DCZC_BUILD_TESTS)
    add_test(NAME depth_filter_smoke COMMAND dczc_depth_filter)
    set_tests_properties(depth_filter_smoke PROPERTIES TIMEOUT 30)
endif()
