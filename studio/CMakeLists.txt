cmake_minimum_required(VERSION 3.22)

project(CoreVideoStudioNative LANGUAGES CXX)

add_executable(CoreVideoStudio WIN32
  src/main.cpp
)

target_compile_features(CoreVideoStudio PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(CoreVideoStudio PRIVATE /W4 /permissive-)
endif()

target_compile_definitions(CoreVideoStudio PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

