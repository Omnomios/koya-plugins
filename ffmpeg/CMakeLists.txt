cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0048 NEW)

set(PROJECT_NAME "sm-ffmpeg")

project(${PROJECT_NAME} VERSION 0.0.1 DESCRIPTION "Koya FFmpeg Test Module (texture update loop)")

set(PROJECT_SOURCE_DIR  "src")
file(GLOB_RECURSE MAIN_SRC "${PROJECT_SOURCE_DIR}/*.cpp")

include_directories(src ../../. ../../sdk)

add_library(${PROJECT_NAME} SHARED ${MAIN_SRC})

if(UNIX OR MINGW)
  find_package(Threads)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
  pkg_check_modules(AVCODEC  REQUIRED IMPORTED_TARGET libavcodec)
  pkg_check_modules(AVUTIL   REQUIRED IMPORTED_TARGET libavutil)
  pkg_check_modules(SWSCALE  REQUIRED IMPORTED_TARGET libswscale)
  target_link_libraries(${PROJECT_NAME} Threads::Threads
    PkgConfig::AVFORMAT PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::SWSCALE)
endif()

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
