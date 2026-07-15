# axon Python bindings (pybind11). Enabled with -DAXON_BUILD_PYTHON=ON.
# Import name is `axon`; the CMake target is `axon_python` to avoid clashing with
# the core `axon` C++ library target.

# pybind11's modern tooling needs the Development.Module component (it provides
# python3_add_library), not just the interpreter.
find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(axon_python axon_py.cpp)
target_link_libraries(axon_python PRIVATE axon)
set_target_properties(axon_python PROPERTIES OUTPUT_NAME axon)

# Fork-based end-to-end test that mirrors tests/test_end_to_end.cpp from Python.
if(AXON_BUILD_TESTS)
    add_test(NAME python_binding
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_axon.py)
    set_tests_properties(python_binding PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:axon_python>")

    # Device (GPU) CUDA-Array-Interface binding. SKIPs cleanly (exit 0) when the
    # module was built without AXON_WITH_CUDA or no CUDA device is present.
    add_test(NAME python_device_array
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_device_array.py)
    set_tests_properties(python_device_array PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:axon_python>")

    # Consumer-side device import + CUDA-Array-Interface roundtrip (A-2). Also
    # SKIPs cleanly without CUDA.
    add_test(NAME python_device_consumer
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_device_consumer.py)
    set_tests_properties(python_device_consumer PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:axon_python>")
endif()
