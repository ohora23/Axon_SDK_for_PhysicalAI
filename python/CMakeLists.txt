# dczc Python bindings (pybind11). Enabled with -DDCZC_BUILD_PYTHON=ON.
# Import name is `dczc`; the CMake target is `dczc_python` to avoid clashing with
# the core `dczc` C++ library target.

# pybind11's modern tooling needs the Development.Module component (it provides
# python3_add_library), not just the interpreter.
find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(dczc_python dczc_py.cpp)
target_link_libraries(dczc_python PRIVATE dczc)
set_target_properties(dczc_python PROPERTIES OUTPUT_NAME dczc)

# Fork-based end-to-end test that mirrors tests/test_end_to_end.cpp from Python.
if(DCZC_BUILD_TESTS)
    add_test(NAME python_binding
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dczc.py)
    set_tests_properties(python_binding PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:dczc_python>")
endif()
