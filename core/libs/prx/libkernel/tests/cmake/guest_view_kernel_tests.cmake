if(BUILD_TESTING AND WIN32)
    add_test_executable(guest_view_kernel_tests ${LIBS_INCLUDE_DIR}/tests/GuestViewKernel.cpp)
    target_include_directories(guest_view_kernel_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_view_kernel_tests PRIVATE libkernel libc)
    configure_windows_unwind(guest_view_kernel_tests)
    add_test(NAME guest_view_kernel COMMAND guest_view_kernel_tests)
    set_tests_properties(guest_view_kernel PROPERTIES TIMEOUT 20 ENVIRONMENT "APS5_GUEST_VIEW=1" ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:libkernel>")
endif()
