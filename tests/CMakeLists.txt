# Loopback wire tests. They drive a real omt::Sender over TCP on loopback and
# observe it through the public API — no mocks of the channel or framer.
#
# Requirements:
#   - static build (-DBUILD_SHARED_LIBS=OFF): the shared target can carry
#     unresolved VMX codec symbols when no prebuilt libvmx is provided, while
#     a static archive only pulls in the objects the tests actually reference.
#   - VMX_ROOT pointing at a libvmx checkout: the tests link a purpose-built
#     stub (vmx_test_stub.cpp) compiled against the real vmxcodec.h so the
#     stubbed signatures cannot drift. The stub satisfies the linker; no
#     prebuilt libvmx binary is needed for control tests. With
#     LIBOMTCPP_TEST_REAL_VMX=ON, media tests compile and run the real codec.

if(BUILD_SHARED_LIBS)
    message(FATAL_ERROR "LIBOMTCPP_BUILD_TESTS requires -DBUILD_SHARED_LIBS=OFF")
endif()

if(NOT VMX_ROOT)
    message(FATAL_ERROR "LIBOMTCPP_BUILD_TESTS requires -DVMX_ROOT=/path/to/libvmx (for vmxcodec.h; no libvmx binary is linked)")
endif()

add_library(vmx_test_stub STATIC vmx_test_stub.cpp)
target_include_directories(vmx_test_stub PUBLIC ${VMX_ROOT}/src)

function(omt_add_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(${name} PRIVATE omt vmx_test_stub)
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    else()
        target_link_libraries(${name} PRIVATE pthread)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

omt_add_test(test_tally)
omt_add_test(test_socket_write)
omt_add_test(test_sender_ports)
omt_add_test(test_audio)
set_tests_properties(test_audio test_tally PROPERTIES TIMEOUT 30)
set_tests_properties(test_socket_write PROPERTIES TIMEOUT 30)

option(LIBOMTCPP_TEST_REAL_VMX "Build media wire tests with the real codec" OFF)
if(LIBOMTCPP_TEST_REAL_VMX)
    add_library(vmx_test_real STATIC
        ${VMX_ROOT}/src/vmxcodec.cpp
        ${VMX_ROOT}/src/vmxcodec_x86.cpp
        ${VMX_ROOT}/src/vmxcodec_avx2.cpp)
    target_include_directories(vmx_test_real PUBLIC ${VMX_ROOT}/src)
    if(MSVC)
        target_compile_options(vmx_test_real PRIVATE /arch:AVX2 /O2)
    else()
        target_compile_options(vmx_test_real PRIVATE -march=haswell -O3 -fno-strict-aliasing)
    endif()
    add_executable(test_media test_media.cpp)
    target_include_directories(test_media PRIVATE ../src)
    target_link_libraries(test_media PRIVATE omt vmx_test_real)
    add_test(NAME test_media COMMAND test_media)
    set_tests_properties(test_media PROPERTIES TIMEOUT 30)
    add_executable(test_receiver test_receiver.cpp)
    target_include_directories(test_receiver PRIVATE ../src)
    target_link_libraries(test_receiver PRIVATE omt vmx_test_real)
    add_test(NAME test_receiver COMMAND test_receiver)
    set_tests_properties(test_receiver PROPERTIES TIMEOUT 45)
endif()
