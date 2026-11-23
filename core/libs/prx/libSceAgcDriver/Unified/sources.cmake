if(ANYPS5_GPU_BACKEND STREQUAL "unified")
    add_compile_definitions(ANYPS5_UNIFIED=1)
    file(GLOB agcUnifiedSources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Unified/src/*.cpp)
    list(APPEND agcDriverSources ${agcUnifiedSources})
    set(agcUnifiedShaderDir ${CMAKE_CURRENT_BINARY_DIR}/generated/prx/libSceAgcDriver/Unified/shaders)
    set(agcUnifiedShaders "DccClear.comp|DccClear|DCC_CLEAR_SPV" "DccClearVolume.comp|DccClearVolume|DCC_CLEAR_VOLUME_SPV""HtileScan.comp|HtileScan|HTILE_SCAN_SPV" "HtileClear.vert|HtileClearVert|HTILE_CLEAR_VERT_SPV")
    foreach(agcUnifiedShader IN LISTS agcUnifiedShaders)
        string(REPLACE "|" ";" agcUnifiedShaderParts "${agcUnifiedShader}")
        list(GET agcUnifiedShaderParts 0 agcUnifiedShaderFile)
        list(GET agcUnifiedShaderParts 1 agcUnifiedShaderName)
        list(GET agcUnifiedShaderParts 2 agcUnifiedShaderSymbol)
        set(agcUnifiedShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/Unified/shaders/${agcUnifiedShaderFile})
        set(agcUnifiedShaderSpv ${agcUnifiedShaderDir}/${agcUnifiedShaderName}.spv)
        set(agcUnifiedShaderHeader ${agcUnifiedShaderDir}/${agcUnifiedShaderName}_spv.h)
        add_custom_command(
                OUTPUT "${agcUnifiedShaderHeader}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${agcUnifiedShaderDir}"
                COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcUnifiedShaderSpv}" "${agcUnifiedShaderSource}"
                COMMAND ${CMAKE_COMMAND} -DINPUT=${agcUnifiedShaderSpv} -DOUTPUT=${agcUnifiedShaderHeader}
                -DSYMBOL=${agcUnifiedShaderSymbol} -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
                DEPENDS "${agcUnifiedShaderSource}" glslang-standalone
                VERBATIM
        )
        list(APPEND agcDriverSources ${agcUnifiedShaderHeader})
    endforeach()
endif()
