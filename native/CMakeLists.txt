cmake_minimum_required(VERSION 3.20)

project(CoreVideoNativeMediaCore LANGUAGES CXX)

option(COREVIDEO_STUB "Build portable stub media-core modules" ON)
option(COREVIDEO_ENABLE_DEV_ADAPTERS "Build SDK/GPU/hardware adapters that require a dev machine" OFF)
option(COREVIDEO_WITH_ZOOM "Build the Zoom Meeting SDK capture adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS and a local Windows SDK root." OFF)
set(COREVIDEO_ZOOM_SDK_ROOT "" CACHE PATH "Root of the extracted Zoom Windows Meeting SDK architecture folder, for example zoom-sdk-windows-7.0.5.39292/x64")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR})
endforeach()

add_library(corevideo_native
  src/core/MediaCore.cpp
  src/modules/StubModules.cpp
  src/modules/ZoomMeetingSdkAdapter.cpp
  src/rpc/Json.cpp
  src/rpc/JsonRpcServer.cpp
)

target_include_directories(corevideo_native PUBLIC src)
target_compile_definitions(corevideo_native
  PUBLIC
    COREVIDEO_STUB=$<BOOL:${COREVIDEO_STUB}>
    COREVIDEO_WITH_ZOOM=$<BOOL:${COREVIDEO_WITH_ZOOM}>
)

if(COREVIDEO_ENABLE_DEV_ADAPTERS)
  target_sources(corevideo_native PRIVATE src/modules/PlatformAdapters.cpp)
  target_compile_definitions(corevideo_native PUBLIC COREVIDEO_ENABLE_DEV_ADAPTERS=1)
endif()

if(COREVIDEO_WITH_ZOOM)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_ZOOM requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(NOT WIN32)
    message(FATAL_ERROR "COREVIDEO_WITH_ZOOM currently supports the Windows Meeting SDK adapter only.")
  endif()
  if(NOT COREVIDEO_ZOOM_SDK_ROOT)
    message(FATAL_ERROR "COREVIDEO_WITH_ZOOM requires -DCOREVIDEO_ZOOM_SDK_ROOT=<zoom-sdk-windows-*/x64>.")
  endif()

  set(COREVIDEO_ZOOM_SDK_INCLUDE_DIR "${COREVIDEO_ZOOM_SDK_ROOT}/h")
  set(COREVIDEO_ZOOM_SDK_LIBRARY "${COREVIDEO_ZOOM_SDK_ROOT}/lib/sdk.lib")
  set(COREVIDEO_ZOOM_SDK_RUNTIME "${COREVIDEO_ZOOM_SDK_ROOT}/bin/sdk.dll")

  foreach(required_path
      "${COREVIDEO_ZOOM_SDK_INCLUDE_DIR}/zoom_sdk.h"
      "${COREVIDEO_ZOOM_SDK_INCLUDE_DIR}/meeting_service_interface.h"
      "${COREVIDEO_ZOOM_SDK_INCLUDE_DIR}/rawdata/zoom_rawdata_api.h"
      "${COREVIDEO_ZOOM_SDK_INCLUDE_DIR}/rawdata/rawdata_renderer_interface.h"
      "${COREVIDEO_ZOOM_SDK_INCLUDE_DIR}/rawdata/rawdata_audio_helper_interface.h"
      "${COREVIDEO_ZOOM_SDK_LIBRARY}"
      "${COREVIDEO_ZOOM_SDK_RUNTIME}")
    if(NOT EXISTS "${required_path}")
      message(FATAL_ERROR "COREVIDEO_WITH_ZOOM missing required Zoom SDK file: ${required_path}")
    endif()
  endforeach()

  target_include_directories(corevideo_native PRIVATE "${COREVIDEO_ZOOM_SDK_INCLUDE_DIR}")
  target_link_libraries(corevideo_native PRIVATE "${COREVIDEO_ZOOM_SDK_LIBRARY}")
  if(MSVC)
    target_link_options(corevideo_native PUBLIC "/DELAYLOAD:sdk.dll")
    target_link_libraries(corevideo_native PUBLIC delayimp)
  endif()
  target_compile_definitions(corevideo_native PUBLIC COREVIDEO_ZOOM_SDK_ROOT="${COREVIDEO_ZOOM_SDK_ROOT}")
endif()

add_executable(corevideo-native src/main.cpp)
target_link_libraries(corevideo-native PRIVATE corevideo_native)

include(CTest)
if(BUILD_TESTING)
  find_package(GTest QUIET)

  add_executable(corevideo-native-tests
    tests/TestMain.cpp
    tests/ContractParityTest.cpp
    tests/JsonRpcServerTest.cpp
    tests/MediaCoreCommandTest.cpp
  )
  target_link_libraries(corevideo-native-tests PRIVATE corevideo_native)
  target_compile_definitions(corevideo-native-tests PRIVATE COREVIDEO_REPO_ROOT="${CMAKE_SOURCE_DIR}/..")
  if(GTest_FOUND)
    target_compile_definitions(corevideo-native-tests PRIVATE COREVIDEO_USE_SYSTEM_GTEST=1)
    target_link_libraries(corevideo-native-tests PRIVATE GTest::gtest)
  else()
    target_include_directories(corevideo-native-tests PRIVATE tests)
  endif()
  if(WIN32)
    add_test(NAME corevideo-native-tests COMMAND "${CMAKE_BINARY_DIR}/corevideo-native-tests.exe")
  else()
    add_test(NAME corevideo-native-tests COMMAND "${CMAKE_BINARY_DIR}/corevideo-native-tests")
  endif()
  if(COREVIDEO_WITH_ZOOM)
    set_tests_properties(corevideo-native-tests PROPERTIES ENVIRONMENT "PATH=${COREVIDEO_ZOOM_SDK_ROOT}/bin;$ENV{PATH}")
  endif()
endif()
