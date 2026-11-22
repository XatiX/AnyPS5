if(ANYPS5_GPU_BACKEND STREQUAL "unified")
    add_compile_definitions(ANYPS5_UNIFIED=1)
    file(GLOB agcUnifiedSources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Unified/src/*.cpp)
    list(APPEND agcDriverSources ${agcUnifiedSources})
    set(agcUnifiedShaderDir ${CMAKE_CURRENT_BINARY_DIR}/generated/prx/libSceAgcDriver/Unified/shaders)
    set(agcDccClearSource ${CMAKE_CURRENT_SOURCE_DIR}/Unified/shaders/DccClear.comp)
    set(agcDccClearSpv ${agcUnifiedShaderDir}/DccClear.spv)
    set(agcDccClearHeader ${agcUnifiedShaderDir}/DccClear_spv.h)
    add_custom_command(
            OUTPUT "${agcDccClearHeader}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${agcUnifiedShaderDir}"
            COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcDccClearSpv}" "${agcDccClearSource}"
            COMMAND ${CMAKE_COMMAND} -DINPUT=${agcDccClearSpv} -DOUTPUT=${agcDccClearHeader}
            -DSYMBOL=DCC_CLEAR_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
            DEPENDS "${agcDccClearSource}" glslang-standalone
            VERBATIM
    )
    list(APPEND agcDriverSources ${agcDccClearHeader})
endif()
