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
option(COREVIDEO_WITH_UVC "Build the native UVC (Media Foundation) capture-device adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS." OFF)
option(COREVIDEO_WITH_WGC "Build the Windows.Graphics.Capture screen-capture adapter. Requires COREVIDEO_ENABLE_DEV_ADAPTERS." OFF)
option(COREVIDEO_WITH_VIRTUALCAM "Build the Windows 11 Media Foundation virtual-camera publisher. Requires COREVIDEO_ENABLE_DEV_ADAPTERS." OFF)
option(COREVIDEO_WITH_BROWSER_HOST "Build corevideo-browser-host.exe (WebView2 browser-source host, docs/capture-sources-spec.md Phase BR). Windows x64 only; uses the vendored third_party/webview2 SDK." OFF)
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

# Emit debug symbols (PDBs) even in Release so production crash dumps are
# diagnosable. Without this a shipping core has NO symbols and a dump reads only as
# raw module+offset (exactly what bit the 2026-07-10 startup crash). /Zi generates the
# PDB, /DEBUG keeps it, and /OPT:REF + /OPT:ICF claw back the code-size/perf cost that
# /DEBUG otherwise disables, so the binary stays as lean as a plain Release. Directory
# scope => applies to every target defined below (core, exe, virtualcam, zoom-engine).
if(MSVC)
  add_compile_options($<$<CONFIG:Release>:/Zi>)
  add_link_options($<$<CONFIG:Release>:/DEBUG>)
  add_link_options($<$<CONFIG:Release>:/OPT:REF>)
  add_link_options($<$<CONFIG:Release>:/OPT:ICF>)
endif()

add_library(corevideo_native
  src/core/LockHoldGuardrail.cpp
  src/core/MediaCore.cpp
  src/modules/AsyncEncoderSink.cpp
  src/modules/BrowserSourceHostAdapter.cpp
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
  src/modules/StillMediaFrameCache.cpp
  src/modules/StubRecordingEncoder.cpp
  src/modules/StubModules.cpp
  src/modules/UvcCaptureDeviceAdapter.cpp
  src/modules/WgcScreenCaptureAdapter.cpp
  src/modules/VirtualCameraPublisher.cpp
  src/modules/UvcCaptureSupport.cpp
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
target_include_directories(corevideo_native PUBLIC plugin-host)
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
    COREVIDEO_WITH_UVC=$<BOOL:${COREVIDEO_WITH_UVC}>
    COREVIDEO_WITH_WGC=$<BOOL:${COREVIDEO_WITH_WGC}>
    COREVIDEO_WITH_VIRTUALCAM=$<BOOL:${COREVIDEO_WITH_VIRTUALCAM}>
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
# windowscodecs + ole32 back the still-media WIC decoder (StillMediaFrameCache.cpp),
# which is _WIN32-gated rather than option-gated so stills work in every Windows build.
if(WIN32)
  target_link_libraries(corevideo_native PUBLIC winmm windowscodecs ole32)
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

if(COREVIDEO_WITH_UVC)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_UVC requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(NOT WIN32)
    message(FATAL_ERROR "COREVIDEO_WITH_UVC currently supports the Windows Media Foundation adapter only.")
  endif()

  # mf provides MFEnumDeviceSources/MFCreateDeviceSource; mfreadwrite the source
  # reader; mfplat/mfuuid the core MF runtime + GUIDs; ole32 COM init.
  target_link_libraries(corevideo_native PUBLIC mfplat mf mfreadwrite mfuuid ole32)
endif()

if(COREVIDEO_WITH_VIRTUALCAM)
  if(NOT WIN32)
    message(FATAL_ERROR "COREVIDEO_WITH_VIRTUALCAM supports Windows only.")
  endif()
  # MFCreateVirtualCamera + MF runtime.
  target_link_libraries(corevideo_native PUBLIC mfplat mf mfsensorgroup ole32)

  # V2b: the user-mode COM DLL the Frame Server loads (IMFMediaSource over the
  # shared-memory slot). No kernel driver, no signing. Registered via regsvr32.
  add_library(corevideo-virtualcam SHARED
      virtualcam-dll/dllmain.cpp
      virtualcam-dll/Guids.cpp
      virtualcam-dll/MediaSource.cpp
      virtualcam-dll/MediaStream.cpp)
  target_compile_features(corevideo-virtualcam PRIVATE cxx_std_17)
  # Win11 target: unlock the MF Frame Server camera attributes
  # (MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES etc. are gated on NTDDI >= RS2).
  target_compile_definitions(corevideo-virtualcam PRIVATE
      WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000C)
  target_include_directories(corevideo-virtualcam PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/virtualcam-dll)
  target_link_libraries(corevideo-virtualcam PRIVATE
      mfplat mf mfuuid mfsensorgroup ole32 oleaut32 ksuser strmiids uuid)
  set_target_properties(corevideo-virtualcam PROPERTIES
      LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/virtualcam-dll/exports.def\"")
endif()

# ---------------------------------------------------------------------------
# corevideo-browser-host — one WebView2 render process per browser source (BR-1).
# Renders --url offscreen, WGC-self-captures its window, publishes BGRA frames
# into the named capture SHM the core maps (src/modules/BrowserSourceShm.h).
# Uses the VENDORED WebView2 SDK (third_party/webview2 — provenance/license in
# its README.md); the WebView2 *Runtime* is probed at host startup.
# ---------------------------------------------------------------------------
if(COREVIDEO_WITH_BROWSER_HOST)
  if(NOT WIN32)
    message(FATAL_ERROR "COREVIDEO_WITH_BROWSER_HOST supports Windows only.")
  endif()
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "COREVIDEO_WITH_BROWSER_HOST requires an x64 build (vendored WebView2LoaderStatic.lib is x64).")
  endif()
  set(COREVIDEO_WEBVIEW2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/webview2")
  if(NOT EXISTS "${COREVIDEO_WEBVIEW2_DIR}/include/WebView2.h")
    message(FATAL_ERROR "Vendored WebView2 SDK missing at ${COREVIDEO_WEBVIEW2_DIR} (see its README.md).")
  endif()
  add_executable(corevideo-browser-host browser-host/main.cpp)
  set_target_properties(corevideo-browser-host PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_include_directories(corevideo-browser-host PRIVATE
    src
    "${COREVIDEO_WEBVIEW2_DIR}/include")
  # windowsapp = C++/WinRT (Windows.Graphics.Capture); version+advapi32 are the
  # WebView2 static loader's documented link requirements.
  target_link_libraries(corevideo-browser-host PRIVATE
    "${COREVIDEO_WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib"
    windowsapp d3d11 dxgi user32 ole32 oleaut32 version advapi32 shlwapi)
endif()

if(COREVIDEO_WITH_WGC)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_WGC requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(NOT WIN32)
    message(FATAL_ERROR "COREVIDEO_WITH_WGC supports Windows only.")
  endif()
  # C++/WinRT consumption of Windows.Graphics.Capture + D3D11 interop.
  target_link_libraries(corevideo_native PUBLIC windowsapp d3d11 dxgi)
endif()

if(COREVIDEO_WITH_WASAPI_MONITOR)
  if(NOT COREVIDEO_ENABLE_DEV_ADAPTERS)
    message(FATAL_ERROR "COREVIDEO_WITH_WASAPI_MONITOR requires COREVIDEO_ENABLE_DEV_ADAPTERS=ON.")
  endif()
  if(NOT WIN32)
    message(FATAL_ERROR "COREVIDEO_WITH_WASAPI_MONITOR currently supports the Windows WASAPI adapter only.")
  endif()

  # avrt = MMCSS (AvSetMmThreadCharacteristics) for the monitor render thread.
  target_link_libraries(corevideo_native PUBLIC ole32 avrt)
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
# corevideo-plugin-host - out-of-process VST3 host (docs/vst-host-spec.md).
# P1: --scan discovery only; pure std::filesystem, NO VST3 SDK dependency, so
# it builds in every configuration (stub, CI, full).
add_executable(corevideo-plugin-host plugin-host/plugin-host.cpp)
set_target_properties(corevideo-plugin-host PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

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
    tests/AsyncEncoderSinkTest.cpp
    tests/AudioDspTest.cpp
    tests/BrowserSourceTest.cpp
    tests/AudioMasteringTest.cpp
    tests/VirtualCameraFrameTest.cpp
    tests/VirtualCameraSlateTest.cpp
    tests/VirtualCameraComposeTest.cpp
    tests/MasteringReferenceTest.cpp
    tests/ImageResizeTest.cpp
    tests/VirtualCameraShmRoundtripTest.cpp
    tests/WgcScreenCaptureTest.cpp
    tests/CaptureIngestTest.cpp
    tests/CompositorFramingTest.cpp
    tests/ContractParityTest.cpp
    tests/D3D11CompositorTest.cpp
    tests/DirectorTest.cpp
    tests/EncoderRecordingSessionTest.cpp
    tests/JsonRpcServerTest.cpp
    tests/LockHoldGuardrailTest.cpp
    tests/MultiviewLayoutTest.cpp
    tests/MediaCoreCommandTest.cpp
    tests/OverlayTileRasterTest.cpp
    tests/NdiSourceNameTest.cpp
    tests/RtmpCompatibilityTest.cpp
    tests/RtmpFfmpegArgsTest.cpp
    tests/RealZoomCaptureSourceTest.cpp
    tests/StillMediaFrameCacheTest.cpp
    tests/UvcCaptureSupportTest.cpp
    tests/VstHostAbiTest.cpp
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
    COREVIDEO_WITH_AJA=$<BOOL:${COREVIDEO_WITH_AJA}>
    COREVIDEO_WITH_UVC=$<BOOL:${COREVIDEO_WITH_UVC}>)
  target_include_directories(corevideo-native-tests PRIVATE zoom-engine/shared plugin-host virtualcam-dll)
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
