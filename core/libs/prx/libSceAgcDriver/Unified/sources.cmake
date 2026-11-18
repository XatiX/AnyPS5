if(ANYPS5_GPU_BACKEND STREQUAL "unified")
    file(GLOB agcUnifiedSources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Unified/src/*.cpp)
    list(APPEND agcDriverSources ${agcUnifiedSources})
endif()
