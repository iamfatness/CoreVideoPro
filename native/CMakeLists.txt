cmake_minimum_required(VERSION 3.20)

project(CoreVideoNativeMediaCore LANGUAGES CXX)

option(COREVIDEO_STUB "Build portable stub media-core modules" ON)
option(COREVIDEO_ENABLE_DEV_ADAPTERS "Build SDK/GPU/hardware adapters that require a dev machine" OFF)
option(COREVIDEO_WITH_ZOOM "Build the Zoom Meeting SDK capture adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS and a local Windows SDK root." OFF)
option(COREVIDEO_WITH_D3D11 "Build the Windows D3D11 GPU compositor adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS." OFF)
option(COREVIDEO_WITH_MF_ENCODER "Build the Windows Media Foundation hardware encoder adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS." OFF)
option(COREVIDEO_WITH_WASAPI_MONITOR "Build the Windows WASAPI audio monitor output adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS." OFF)
option(COREVIDEO_WITH_WASAPI_CAPTURE "Build the Windows WASAPI audio capture adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS." OFF)
option(COREVIDEO_WITH_RTMP_OUTPUT "Build the RTMP output sender adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS and a libavformat runtime on the dev machine." OFF)
option(COREVIDEO_WITH_SRT_OUTPUT "Build the SRT output sender adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS and Haivision libsrt on the dev machine." OFF)
option(COREVIDEO_WITH_NDI_OUTPUT "Build the NDI output sender adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS and a libNDI runtime on the dev machine (runtime-probed)." OFF)
option(COREVIDEO_WITH_SRT_INGEST "Build the SRT ingest capture adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS and Haivision libsrt on the dev machine for real transport." OFF)
option(COREVIDEO_WITH_DECKLINK "Build the Blackmagic DeckLink capture-device adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS and a dev-machine runtime." OFF)
option(COREVIDEO_WITH_AJA "Build the AJA NTV2 capture-device adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS and a dev-machine runtime." OFF)
option(COREVIDEO_BUILD_ZOOM_ENGINE "Build the vendored standalone Zoom capture engine helper. Requires COREVIDEO_ENABLE_DEV_ADAPTERS and a local Zoom Meeting SDK." OFF)
set(COREVIDEO_ZOOM_SDK_ROOT "" CACHE PATH "Root of the extracted Zoom Windows Meeting SDK architecture folder, for example zoom-sdk-windows-7.0.5.39292/x64")
set(ZOOM_SDK_DIR "" CACHE PATH "Path to the vendored Zoom capture engine SDK root (contains include/ or h/, plus lib/)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR})
endforeach()

add_library(corevideo_native
  src/core/MediaCore.cpp
  src/modules/D3D11CompositorAdapter.cpp
  src/modules/HardwareCaptureDeviceAdapters.cpp
  src/modules/MediaFoundationEncoderAdapter.cpp
  src/modules/MediaFoundationMediaFrameSourceAdapter.cpp
  src/modules/NdiOutputSenderAdapter.cpp
  src/modules/OverlayTileRaster.cpp
  src/modules/ProgramFramePreview.cpp
  src/modules/RealZoomCaptureSource.cpp
  src/modules/RtmpOutputSenderAdapter.cpp
  src/modules/SrtIngestCaptureAdapter.cpp
  src/modules/SrtOutputSenderAdapter.cpp
  src/modules/StubRecordingEncoder.cpp
  src/modules/StubModules.cpp
  src/modules/WasapiAudioCaptureSourceAdapter.cpp
  src/modules/WasapiMonitorOutputAdapter.cpp
  src/modules/WinUiCaptureDeviceAdapter.cpp
  src/modules/ZoomEngineClient.cpp
  src/modules/ZoomEngineProcess.cpp
  src/modules/ZoomEngineRuntime.cpp
  src/modules/ZoomEngineState.cpp
  src/modules/ZoomMeetingSdkAdapter.cpp
  src/rpc/Json.cpp
  src/rpc/JsonRpcServer.cpp
  src/zoom/I420Convert.cpp
  src/zoom/ShmFrameReader.cpp
)

target_include_directories(corevideo_native PUBLIC src)
target_include_directories(corevideo_native PRIVATE zoom-engine/shared)
target_compile_definitions(corevideo_native
  PUBLIC
    COREVIDEO_STUB=$<BOOL:${COREVIDEO_STUB}>
    COREVIDEO_WITH_ZOOM=$<BOOL:${COREVIDEO_WITH_ZOOM}>
    COREVIDEO_WITH_D3D11=$<BOOL:${COREVIDEO_WITH_D3D11}>
    COREVIDEO_WITH_MF_ENCODER=$<BOOL:${COREVIDEO_WITH_MF_ENCODER}>
    COREVIDEO_WITH_WASAPI_MONITOR=$<BOOL:${COREVIDEO_WITH_WASAPI_MONITOR}>
    COREVIDEO_WITH_WASAPI_CAPTURE=$<BOOL:${COREVIDEO_WITH_WASAPI_CAPTURE}>
    COREVIDEO_WITH_RTMP_OUTPUT=$<BOOL:${COREVIDEO_WITH_RTMP_OUTPUT}>
    COREVIDEO_WITH_SRT_OUTPUT=$<BOOL:${COREVIDEO_WITH_SRT_OUTPUT}>
    COREVIDEO_WITH_NDI_OUTPUT=$<BOOL:${COREVIDEO_WITH_NDI_OUTPUT}>
    COREVIDEO_WITH_SRT_INGEST=$<BOOL:${COREVIDEO_WITH_SRT_INGEST}>
    COREVIDEO_WITH_DECKLINK=$<BOOL:${COREVIDEO_WITH_DECKLINK}>
    COREVIDEO_WITH_AJA=$<BOOL:${COREVIDEO_WITH_AJA}>
)

if(COREVIDEO_ENABLE_DEV_ADAPTERS)
  target_sources(corevideo_native PRIVATE src/modules/PlatformAdapters.cpp)
  target_compile_definitions(corevideo_native PUBLIC COREVIDEO_ENABLE_DEV_ADAPTERS=1)
endif()

if(COREVIDEO_WITH_D3D11)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_D3D11 requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(NOT WIN32)
    message(FATAL_ERROR "COREVIDEO_WITH_D3D11 currently supports the Windows D3D11 adapter only.")
  endif()

  # d2d1/dwrite raster real overlay text into a GPU texture; windowscodecs (WIC)
  # decodes overlay images; ole32 provides CoInitializeEx for the WIC factory.
  target_link_libraries(corevideo_native PUBLIC d3d11 dxgi d3dcompiler d2d1 dwrite windowscodecs ole32)
endif()

# winmm provides timeBeginPeriod() — raise the system timer resolution to 1ms so the
# render loop's sub-frame sleep is accurate (default ~15.6ms granularity caps fps ~40).
if(WIN32)
  target_link_libraries(corevideo_native PUBLIC winmm)
endif()

if(COREVIDEO_WITH_MF_ENCODER)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_MF_ENCODER requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(NOT WIN32)
    message(FATAL_ERROR "COREVIDEO_WITH_MF_ENCODER currently supports the Windows Media Foundation adapter only.")
  endif()

  target_link_libraries(corevideo_native PUBLIC mfplat mf mfreadwrite mfuuid ole32 windowscodecs)
endif()

if(COREVIDEO_WITH_WASAPI_MONITOR)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_WASAPI_MONITOR requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(NOT WIN32)
    message(FATAL_ERROR "COREVIDEO_WITH_WASAPI_MONITOR currently supports the Windows WASAPI adapter only.")
  endif()

  target_link_libraries(corevideo_native PUBLIC ole32)
endif()

if(COREVIDEO_WITH_WASAPI_CAPTURE)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_WASAPI_CAPTURE requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(NOT WIN32)
    message(FATAL_ERROR "COREVIDEO_WITH_WASAPI_CAPTURE currently supports the Windows WASAPI adapter only.")
  endif()

  target_link_libraries(corevideo_native PUBLIC ole32)
endif()

if(COREVIDEO_WITH_RTMP_OUTPUT)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_RTMP_OUTPUT requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(corevideo_native PUBLIC dl)
  endif()
endif()

if(COREVIDEO_WITH_NDI_OUTPUT)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_NDI_OUTPUT requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  # The adapter runtime-probes the libNDI shared library (Processing.NDI.Lib on
  # Windows, libndi on POSIX) via LoadLibrary/dlopen, so there is no link-time
  # dependency on the NDI SDK. Only the POSIX dynamic-loader is needed.
  if(UNIX AND NOT APPLE)
    target_link_libraries(corevideo_native PUBLIC dl)
  endif()
endif()

if(COREVIDEO_WITH_SRT_OUTPUT OR COREVIDEO_WITH_SRT_INGEST)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "SRT adapters require COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  find_package(SRT CONFIG QUIET)
  if(TARGET SRT::srt)
    target_link_libraries(corevideo_native PUBLIC SRT::srt)
    target_compile_definitions(corevideo_native PUBLIC COREVIDEO_HAS_LIBSRT=1)
  else()
    message(WARNING "SRT adapter enabled but SRT::srt was not found. Building runtime-probe scaffold only; set CMAKE_PREFIX_PATH or use vcpkg/conan to provide Haivision libsrt.")
    target_compile_definitions(corevideo_native PUBLIC COREVIDEO_HAS_LIBSRT=0)
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(corevideo_native PUBLIC dl)
  endif()
  if(WIN32)
    target_link_libraries(corevideo_native PUBLIC ws2_32)
  endif()
else()
  target_compile_definitions(corevideo_native PUBLIC COREVIDEO_HAS_LIBSRT=0)
endif()

if(COREVIDEO_WITH_DECKLINK)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_DECKLINK requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(WIN32)
    target_link_libraries(corevideo_native PUBLIC ole32 oleaut32)
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(corevideo_native PUBLIC dl)
  endif()
endif()

if(COREVIDEO_WITH_AJA)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_AJA requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(corevideo_native PUBLIC dl)
  endif()
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

# ---------------------------------------------------------------------------
# Synthetic (fake) Zoom capture engine — NO Zoom SDK dependency.
# Speaks the exact same named-pipe + shared-memory protocol as
# corevideo-zoom-engine, but fabricates a multi-participant meeting so the
# multi-participant crash/freeze scenario can be reproduced and validated
# without a real Zoom call. Built unconditionally (pure Win32/POSIX), so both
# `npm run build:native-dev` and the portable stub build produce it.
# Point the core at it with COREVIDEO_ZOOM_ENGINE_PATH=<...>/corevideo-zoom-engine-fake.
# ---------------------------------------------------------------------------
add_executable(corevideo-zoom-engine-fake zoom-engine/fake/fake-engine.cpp)
target_include_directories(corevideo-zoom-engine-fake PRIVATE
  zoom-engine/shared
  zoom-engine/engine
)
if(WIN32)
  # IPC (named pipes) + SHM (file mappings) use only kernel32 (linked by default).
elseif(UNIX AND NOT APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(corevideo-zoom-engine-fake PRIVATE Threads::Threads rt)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(corevideo-zoom-engine-fake PRIVATE Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Vendored standalone Zoom capture engine.
# Built only on a dev machine that has the Zoom Meeting SDK; excluded from the
# default in-container stub build so `-DCOREVIDEO_STUB=ON` stays green.
# See native/zoom-engine/README.md for provenance and sync instructions.
# ---------------------------------------------------------------------------
if(COREVIDEO_BUILD_ZOOM_ENGINE)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_BUILD_ZOOM_ENGINE requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(NOT ZOOM_SDK_DIR)
    message(FATAL_ERROR "COREVIDEO_BUILD_ZOOM_ENGINE requires -DZOOM_SDK_DIR=<path to Zoom SDK>")
  endif()
  if(EXISTS "${ZOOM_SDK_DIR}/include")
    set(COREVIDEO_ZOOM_ENGINE_INCLUDE_DIR "${ZOOM_SDK_DIR}/include")
  elseif(EXISTS "${ZOOM_SDK_DIR}/h")
    set(COREVIDEO_ZOOM_ENGINE_INCLUDE_DIR "${ZOOM_SDK_DIR}/h")
  else()
    message(FATAL_ERROR "COREVIDEO_BUILD_ZOOM_ENGINE expected include/ or h/ under ZOOM_SDK_DIR=${ZOOM_SDK_DIR}")
  endif()

  add_executable(corevideo-zoom-engine
    zoom-engine/engine/main.cpp
    zoom-engine/engine/engine-video.cpp
    zoom-engine/engine/engine-audio.cpp
    zoom-engine/engine/engine-share.cpp
  )
  target_include_directories(corevideo-zoom-engine PRIVATE
    zoom-engine/engine
    zoom-engine/shared
    "${COREVIDEO_ZOOM_ENGINE_INCLUDE_DIR}"
    "${COREVIDEO_ZOOM_ENGINE_INCLUDE_DIR}/rawdata"
    "${COREVIDEO_ZOOM_ENGINE_INCLUDE_DIR}/meeting_service_components"
  )
  if(WIN32)
    file(GLOB ZOOM_SDK_LIBRARY "${ZOOM_SDK_DIR}/lib/*.lib")
    target_link_libraries(corevideo-zoom-engine PRIVATE
      ws2_32 crypt32 winhttp shlwapi ${ZOOM_SDK_LIBRARY})
    if(EXISTS "${ZOOM_SDK_DIR}/bin")
      add_custom_command(TARGET corevideo-zoom-engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${ZOOM_SDK_DIR}/bin"
          "$<TARGET_FILE_DIR:corevideo-zoom-engine>")
    endif()
  elseif(APPLE)
    target_link_libraries(corevideo-zoom-engine PRIVATE
      "-F${ZOOM_SDK_DIR}/lib" "-framework ZoomSDK"
      "-framework CoreFoundation" "-framework Foundation")
  else()
    target_link_libraries(corevideo-zoom-engine PRIVATE
      "${ZOOM_SDK_DIR}/lib/libsdk.so" rt)
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(GTest QUIET)

  add_executable(corevideo-native-tests
    tests/TestMain.cpp
    tests/AudioDspTest.cpp
    tests/CaptureIngestTest.cpp
    tests/CompositorFramingTest.cpp
    tests/ContractParityTest.cpp
    tests/D3D11CompositorTest.cpp
    tests/DirectorTest.cpp
    tests/EncoderRecordingSessionTest.cpp
    tests/JsonRpcServerTest.cpp
    tests/MultiviewLayoutTest.cpp
    tests/MediaCoreCommandTest.cpp
    tests/OverlayTileRasterTest.cpp
    tests/NdiSourceNameTest.cpp
    tests/RtmpCompatibilityTest.cpp
    tests/RtmpFfmpegArgsTest.cpp
    tests/RealZoomCaptureSourceTest.cpp
    tests/ZoomFrameReaderTest.cpp
    tests/ZoomEngineClientTest.cpp
    tests/ZoomEngineProcessTest.cpp
    tests/ZoomEngineRuntimeTest.cpp
    tests/ZoomEngineStateTest.cpp
  )
  target_link_libraries(corevideo-native-tests PRIVATE corevideo_native)
  target_compile_definitions(corevideo-native-tests PRIVATE
    COREVIDEO_REPO_ROOT="${CMAKE_SOURCE_DIR}/.."
    COREVIDEO_STUB=$<BOOL:${COREVIDEO_STUB}>
    COREVIDEO_WITH_ZOOM=$<BOOL:${COREVIDEO_WITH_ZOOM}>
    COREVIDEO_WITH_D3D11=$<BOOL:${COREVIDEO_WITH_D3D11}>
    COREVIDEO_WITH_MF_ENCODER=$<BOOL:${COREVIDEO_WITH_MF_ENCODER}>
    COREVIDEO_WITH_WASAPI_MONITOR=$<BOOL:${COREVIDEO_WITH_WASAPI_MONITOR}>
    COREVIDEO_WITH_WASAPI_CAPTURE=$<BOOL:${COREVIDEO_WITH_WASAPI_CAPTURE}>
    COREVIDEO_WITH_RTMP_OUTPUT=$<BOOL:${COREVIDEO_WITH_RTMP_OUTPUT}>
    COREVIDEO_WITH_SRT_OUTPUT=$<BOOL:${COREVIDEO_WITH_SRT_OUTPUT}>
    COREVIDEO_WITH_NDI_OUTPUT=$<BOOL:${COREVIDEO_WITH_NDI_OUTPUT}>
    COREVIDEO_WITH_SRT_INGEST=$<BOOL:${COREVIDEO_WITH_SRT_INGEST}>
    COREVIDEO_WITH_DECKLINK=$<BOOL:${COREVIDEO_WITH_DECKLINK}>
    COREVIDEO_WITH_AJA=$<BOOL:${COREVIDEO_WITH_AJA}>)
  target_include_directories(corevideo-native-tests PRIVATE zoom-engine/shared)
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
