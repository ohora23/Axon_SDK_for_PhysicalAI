# Examples — week 1-2 spike PoC is the first citizen.

# Conditional add_subdirectory so the tree configures cleanly even before
# the spike PoC PR has landed.
if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/spike_poc)
    add_subdirectory(spike_poc)
endif()
