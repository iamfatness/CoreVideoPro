cmake_minimum_required(VERSION 3.20)

project(CoreVideoNativeMediaCore LANGUAGES CXX)

option(COREVIDEO_STUB "Build portable stub media-core modules" ON)
option(COREVIDEO_ENABLE_DEV_ADAPTERS "Build SDK/GPU/hardware adapters that require a dev machine" OFF)

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
  src/rpc/Json.cpp
  src/rpc/JsonRpcServer.cpp
)

target_include_directories(corevideo_native PUBLIC src)
target_compile_definitions(corevideo_native
  PUBLIC
    COREVIDEO_STUB=$<BOOL:${COREVIDEO_STUB}>
)

if(COREVIDEO_ENABLE_DEV_ADAPTERS)
  target_sources(corevideo_native PRIVATE src/modules/PlatformAdapters.cpp)
  target_compile_definitions(corevideo_native PUBLIC COREVIDEO_ENABLE_DEV_ADAPTERS=1)
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
endif()
