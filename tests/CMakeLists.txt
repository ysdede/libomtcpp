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
#     libvmx binary is needed because the tests never encode video.

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
