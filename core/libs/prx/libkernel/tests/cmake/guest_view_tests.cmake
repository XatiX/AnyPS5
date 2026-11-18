if(BUILD_TESTING AND WIN32)
    add_test_executable(guest_view_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/GuestView.cpp ${CMAKE_CURRENT_SOURCE_DIR}/DirectMemory/GuestView.cpp)
    target_include_directories(guest_view_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_view_tests PRIVATE libc)
    configure_windows_unwind(guest_view_tests)
    add_test(NAME guest_view COMMAND guest_view_tests)
    set_tests_properties(guest_view PROPERTIES TIMEOUT 20 ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
endif()
