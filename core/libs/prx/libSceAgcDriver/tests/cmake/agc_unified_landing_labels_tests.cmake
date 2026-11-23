if(NOT ANYPS5_GPU_BACKEND STREQUAL "unified")
    return()
endif()
add_test_executable(agc_unified_landing_labels_tests EXCLUDE_FROM_ALL tests/UnifiedLandingLabels.cpp)
target_include_directories(agc_unified_landing_labels_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
        ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
)
target_link_libraries(agc_unified_landing_labels_tests PRIVATE libSceAgcDriver SDL2-static libc)
set_target_properties(agc_unified_landing_labels_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_unified_landing_labels_tests)
target_compile_definitions(agc_unified_landing_labels_tests PRIVATE VK_ENABLE_BETA_EXTENSIONS)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(agc_unified_landing_labels_tests PRIVATE
            -include ${CMAKE_CURRENT_SOURCE_DIR}/tests/execution/TestEnvironment.hpp)
endif()

if(BUILD_TESTING)
    agc_add_default_test(agc_unified_landing_labels_tests)
    set_tests_properties(agc_unified_landing_labels PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_OPPORTUNISTIC_REAP=1" TIMEOUT 30)
endif()
