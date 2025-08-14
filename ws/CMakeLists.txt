cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0048 NEW)

set(PROJECT_NAME "sm-ws")

project(${PROJECT_NAME} VERSION 0.0.1 DESCRIPTION "Syncromesh WebSocket Module")

set(PROJECT_EXTERNAL_DIR  "IXWebSocket")
set(PROJECT_SOURCE_DIR  "src")
file(GLOB_RECURSE MAIN_SRC "${PROJECT_SOURCE_DIR}/*.cpp")

get_filename_component(FINAL_BIN "../../../bin" ABSOLUTE)

include_directories(${PROJECT_EXTERNAL_DIR})
include_directories(src ../../. ../../sdk)

add_library(${PROJECT_NAME} SHARED ${MAIN_SRC})

## Removed custom output directory; use default CMake build output

add_subdirectory("${PROJECT_EXTERNAL_DIR}")
set_target_properties(ixwebsocket PROPERTIES OUTPUT_NAME "$<$<NOT:$<PLATFORM_ID:Windows>>:libixwebsocket>")
set_target_properties(ixwebsocket PROPERTIES VERSION "" SOVERSION "")
target_compile_options(ixwebsocket PRIVATE -fPIC)

if(UNIX OR MINGW)
  find_package(Threads)
  target_link_libraries(${PROJECT_NAME} ixwebsocket Threads::Threads)
endif()

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)