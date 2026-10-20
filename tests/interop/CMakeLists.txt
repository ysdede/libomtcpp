cmake_minimum_required(VERSION 3.20)
project(omt_official_interop LANGUAGES CXX)
if(NOT MSVC OR NOT VMX_ROOT OR NOT LIBOMTNET_ROOT OR NOT CSC_EXECUTABLE)
    message(FATAL_ERROR "Windows x64 requires VMX_ROOT, LIBOMTNET_ROOT and CSC_EXECUTABLE (Roslyn csc.exe)")
endif()
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(LIBOMTCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
add_library(vmx SHARED ${VMX_ROOT}/src/vmxcodec.cpp ${VMX_ROOT}/src/vmxcodec_x86.cpp ${VMX_ROOT}/src/vmxcodec_avx2.cpp)
target_compile_options(vmx PRIVATE /O2 /arch:AVX2)
set_target_properties(vmx PROPERTIES OUTPUT_NAME libvmx)
add_subdirectory(../.. omt-build)
target_link_libraries(omt PRIVATE vmx)
target_sources(omt PRIVATE shim.cpp)
file(GLOB_RECURSE official_sources CONFIGURE_DEPENDS "${LIBOMTNET_ROOT}/src/*.cs")
add_custom_target(official_interop ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/run"
    COMMAND "${CSC_EXECUTABLE}" /nologo /unsafe /optimize+ /platform:x64 "/out:${CMAKE_CURRENT_BINARY_DIR}/run/Interop.exe" ${official_sources} "${CMAKE_CURRENT_LIST_DIR}/Interop.cs"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:omt>" "${CMAKE_CURRENT_BINARY_DIR}/run/omt.dll"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:vmx>" "${CMAKE_CURRENT_BINARY_DIR}/run/libvmx.dll"
    DEPENDS omt vmx VERBATIM)
enable_testing()
add_test(NAME official_interop COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run/Interop.exe")
set_tests_properties(official_interop PROPERTIES TIMEOUT 45)
