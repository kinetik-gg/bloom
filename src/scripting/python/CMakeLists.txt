bloom_find_dependency(nanobind)

# Only native library dependencies of the extension need PIC. Do not change unrelated
# executables, tests, worker binaries or desktop libraries when enabling Python.
function(bloom_python_require_pic bloom_python_target)
    get_target_property(bloom_python_imported "${bloom_python_target}" IMPORTED)
    get_target_property(bloom_python_type "${bloom_python_target}" TYPE)
    get_target_property(bloom_python_visited "${bloom_python_target}" BLOOM_PYTHON_PIC_VISITED)
    if(bloom_python_imported OR bloom_python_visited)
        return()
    endif()
    set_property(TARGET "${bloom_python_target}" PROPERTY BLOOM_PYTHON_PIC_VISITED TRUE)
    if(bloom_python_type STREQUAL "STATIC_LIBRARY" OR bloom_python_type STREQUAL "OBJECT_LIBRARY")
        set_property(TARGET "${bloom_python_target}" PROPERTY POSITION_INDEPENDENT_CODE ON)
    endif()
    get_target_property(bloom_python_links "${bloom_python_target}" LINK_LIBRARIES)
    foreach(bloom_python_link IN LISTS bloom_python_links)
        if(TARGET "${bloom_python_link}")
            bloom_python_require_pic("${bloom_python_link}")
        endif()
    endforeach()
endfunction()
bloom_python_require_pic(bloom_scripting)

add_library(bloom_python_binding OBJECT native.cpp projections.cpp render.cpp bindings.cpp task_ticket.cpp root_retirements.cpp)
target_link_libraries(bloom_python_binding PRIVATE bloom_scripting nanobind::core Python3::Module)
target_compile_features(bloom_python_binding PRIVATE cxx_std_20)
target_compile_definitions(bloom_python_binding PRIVATE BLOOM_PYTHON_VERSION="${PROJECT_VERSION}")
set_target_properties(bloom_python_binding PROPERTIES POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
bloom_enable_warnings(bloom_python_binding)

Python3_add_library(_bloom MODULE WITH_SOABI $<TARGET_OBJECTS:bloom_python_binding>)
target_link_libraries(_bloom PRIVATE bloom_scripting nanobind::core)
set_target_properties(_bloom PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python")
file(GLOB_RECURSE bloom_python_files CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "bloom/*.py" "bloom/*.pyi" "bloom/py.typed")
foreach(bloom_python_file IN LISTS bloom_python_files)
    configure_file("${bloom_python_file}" "${CMAKE_BINARY_DIR}/python/${bloom_python_file}" COPYONLY)
endforeach()
install(TARGETS _bloom LIBRARY DESTINATION lib/bloom/python RUNTIME DESTINATION lib/bloom/python)
configure_file(_bloom.pyi "${CMAKE_BINARY_DIR}/python/_bloom.pyi" COPYONLY)
install(FILES _bloom.pyi DESTINATION lib/bloom/python)
install(DIRECTORY bloom DESTINATION lib/bloom/python PATTERN "__pycache__" EXCLUDE)

add_library(bloom_python_runtime STATIC runtime.cpp $<TARGET_OBJECTS:bloom_python_binding>)
target_link_libraries(bloom_python_runtime PUBLIC bloom_scripting PRIVATE nanobind::core Python3::Python)
target_include_directories(bloom_python_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(bloom_python_runtime PRIVATE
    BLOOM_PYTHON_PACKAGE_PATH="${CMAKE_BINARY_DIR}/python")
bloom_enable_warnings(bloom_python_runtime)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_python_root_retirements_test "${PROJECT_SOURCE_DIR}/tests/scripting/root_retirements_tests.cpp" root_retirements.cpp)
    target_link_libraries(bloom_python_root_retirements_test PRIVATE bloom_scripting bloom_render)
    target_include_directories(bloom_python_root_retirements_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/render" "${CMAKE_CURRENT_SOURCE_DIR}")
    bloom_enable_warnings(bloom_python_root_retirements_test)
    bloom_add_test(NAME bloom.scripting.python-root-retirements
        COMMAND bloom_python_root_retirements_test
        LABELS scripting python host unit)
    add_executable(bloom_python_root_lifecycle_test "${PROJECT_SOURCE_DIR}/tests/scripting/root_lifecycle_tests.cpp")
    target_link_libraries(bloom_python_root_lifecycle_test PRIVATE
        bloom_python_runtime bloom_render nanobind::core Python3::Python)
    target_include_directories(bloom_python_root_lifecycle_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/render" "${CMAKE_CURRENT_SOURCE_DIR}")
    bloom_enable_warnings(bloom_python_root_lifecycle_test)
    bloom_add_test(NAME bloom.scripting.python-root-lifecycle
        COMMAND bloom_python_root_lifecycle_test
        LABELS scripting python host integration)
    bloom_add_test(NAME bloom.scripting.python-root-subprocess
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_root_lifecycle.py"
            "${CMAKE_BINARY_DIR}/python" $<TARGET_FILE:bloom-cli>
        LABELS scripting python host integration)
    bloom_add_test(NAME bloom.scripting.python-api
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_api.py"
            "${CMAKE_BINARY_DIR}/python" $<TARGET_FILE:bloom-cli>
        LABELS scripting python integration device
        # bloom-cli export probes the encoders and renders a saved queue end to end.
        TIMEOUT 180)
    # #357: several Hosts rendering through one interpreter device must exit promptly.
    bloom_add_test(NAME bloom.scripting.python-shared-device
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_shared_device.py"
            "${CMAKE_BINARY_DIR}/python"
        LABELS scripting python integration device)
    bloom_add_test(NAME bloom.scripting.python-embedding
        COMMAND bloom-cli run "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/embedding.py"
        LABELS scripting python integration device)
    # The recipe in docs/user-guide/python.md, run the way an artist runs it.
    bloom_add_test(NAME bloom.scripting.python-first-five-minutes
        COMMAND bloom-cli run "${PROJECT_SOURCE_DIR}/examples/scripting/first_five_minutes.py"
        LABELS scripting python integration device)
endif()
