# dczc closed-loop demo + measurement harness.
# Uses the public dczc C++ API (no camera, no accelerator — Custom memfd pool).

add_executable(dczc_demo dczc_demo.cpp)
target_link_libraries(dczc_demo PRIVATE dczc)

# Smoke test: a short run must complete with zero payload errors and observe the
# final frame. Kept tiny so it is CI-friendly.
if(DCZC_BUILD_TESTS)
    add_test(NAME demo_smoke
             COMMAND dczc_demo --frames 30 --rate-hz 500 --buffers 8 --quiet)
    set_tests_properties(demo_smoke PROPERTIES TIMEOUT 30)
endif()
