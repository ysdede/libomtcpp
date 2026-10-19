cmake_minimum_required(VERSION 3.14)
project(libomtcpp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(BUILD_SHARED_LIBS "Build shared library" ON)
option(LIBOMTCPP_BUILD_EXAMPLES "Build example programs" OFF)
option(LIBOMTCPP_BUILD_TESTS "Build loopback wire tests (requires -DBUILD_SHARED_LIBS=OFF and VMX_ROOT)" OFF)

# VMX Codec dependency (external)
# Usage: cmake -DVMX_ROOT=/path/to/libvmx ..
if(NOT VMX_ROOT)
    message(WARNING "VMX_ROOT not set. Please specify -DVMX_ROOT=/path/to/libvmx")
endif()

# Source files
set(LIBOMTCPP_SOURCES
    src/libomt.cpp
    src/OMTAsyncPool.cpp
    src/OMTChannel.cpp
    src/OMTDiscovery.cpp
    src/OMTSender.cpp
)

set(LIBOMTCPP_HEADERS
    include/libomt.h
    src/OMTAsyncPool.h
    src/OMTChannel.h
    src/OMTConstants.h
    src/OMTDiscovery.h
    src/OMTProtocol.h
    src/OMTSender.h
)

# Library target
add_library(omt ${LIBOMTCPP_SOURCES} ${LIBOMTCPP_HEADERS})

target_include_directories(omt
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# VMX includes and linking
if(VMX_ROOT)
    target_include_directories(omt PRIVATE ${VMX_ROOT}/include ${VMX_ROOT}/src)
    
    # Find VMX library
    find_library(VMX_LIBRARY
        NAMES vmx libvmx
        PATHS ${VMX_ROOT}/lib ${VMX_ROOT}/build
        NO_DEFAULT_PATH
    )
    if(VMX_LIBRARY)
        target_link_libraries(omt PRIVATE ${VMX_LIBRARY})
    endif()
endif()

# Platform-specific settings
if(ANDROID)
    target_compile_definitions(omt PRIVATE OMT_PLATFORM_ANDROID)
    target_compile_options(omt PRIVATE -fms-extensions -fdeclspec)
    if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        target_compile_definitions(omt PRIVATE ARM64)
    endif()
    target_link_libraries(omt PRIVATE log)
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(omt PRIVATE OMT_PLATFORM_LINUX)
    target_link_libraries(omt PRIVATE pthread)
elseif(APPLE)
    target_compile_definitions(omt PRIVATE OMT_PLATFORM_MACOS)
    target_link_libraries(omt PRIVATE pthread)
elseif(WIN32)
    target_compile_definitions(omt PRIVATE OMT_PLATFORM_WINDOWS)
    target_link_libraries(omt PRIVATE ws2_32)
endif()

# Install rules
include(GNUInstallDirs)

install(TARGETS omt
    EXPORT omtTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES include/libomt.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Examples
if(LIBOMTCPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests
if(LIBOMTCPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
