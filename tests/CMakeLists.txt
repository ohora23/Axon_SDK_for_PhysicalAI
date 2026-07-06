# Week 3-4 formal implementation — unit + end-to-end tests.
# Dependency-free harness (tests/axon_test.h); no GTest so the tree builds on a
# bare target board. Each executable is registered with ctest.

set(AXON_TESTS
    test_descriptor
    test_rt
    test_sidecar
    test_seqlock
    test_pool
    test_end_to_end)

foreach(t ${AXON_TESTS})
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE axon)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# The concurrency and e2e tests spawn threads / child processes; give them room.
set_tests_properties(test_seqlock     PROPERTIES TIMEOUT 60)
set_tests_properties(test_end_to_end  PROPERTIES TIMEOUT 60)
