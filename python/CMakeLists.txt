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
endif()
