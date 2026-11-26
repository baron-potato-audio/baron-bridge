# Copyright (C) 2026 Baron Potato
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.22)
project(BaronBridge VERSION 1.0.2 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT MSVC)
    message(FATAL_ERROR "Build Baron Bridge with MSVC for Windows x64.")
endif()
set(BARON_SDK_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/steinberg-sdk"
    CACHE PATH "Directory containing the Steinberg interface headers")
foreach(required_file iasiodrv.h asio.h asiosys.h LICENSE.txt)
    if(NOT EXISTS "${BARON_SDK_HEADERS}/${required_file}")
        message(FATAL_ERROR "Missing ${required_file} in BARON_SDK_HEADERS: ${BARON_SDK_HEADERS}")
    endif()
endforeach()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_library(bridge_endpoints STATIC AudioEndpoints.cpp)
target_link_libraries(bridge_endpoints PUBLIC ole32 user32)
target_compile_features(bridge_endpoints PUBLIC cxx_std_20)
target_compile_definitions(bridge_endpoints PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_compile_options(bridge_endpoints PUBLIC /utf-8)

add_library(BaronBridge SHARED Driver.cpp SettingsDialog.cpp SettingsDialog.rc Bridge.def)
target_include_directories(BaronBridge PRIVATE "${BARON_SDK_HEADERS}")
target_link_libraries(BaronBridge PRIVATE bridge_endpoints avrt winmm)
target_compile_options(BaronBridge PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4>)

add_executable(baron_bridge_check BridgeCheck.cpp)
target_include_directories(baron_bridge_check PRIVATE "${BARON_SDK_HEADERS}")
target_link_libraries(baron_bridge_check PRIVATE bridge_endpoints)
target_compile_options(baron_bridge_check PRIVATE /W4)

install(TARGETS BaronBridge RUNTIME DESTINATION .)
install(FILES COPYING.txt THIRD-PARTY-NOTICES.txt DESTINATION .)
install(FILES "${BARON_SDK_HEADERS}/LICENSE.txt" DESTINATION . RENAME Steinberg-SDK-LICENSE.txt)
install(FILES third_party/inno-setup/LICENSE.txt DESTINATION . RENAME Inno-Setup-LICENSE.txt)
