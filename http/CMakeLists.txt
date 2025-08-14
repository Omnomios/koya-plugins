cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0048 NEW)

set(PROJECT_NAME "sm-http")

project(${PROJECT_NAME} VERSION 0.0.1 DESCRIPTION "Syncromesh HTTP Module")

set(PROJECT_SOURCE_DIR  "src")
file(GLOB_RECURSE MAIN_SRC "${PROJECT_SOURCE_DIR}/*.cpp")

include_directories(cpp-httplib src ../../. ../../sdk)

add_library(${PROJECT_NAME} SHARED ${MAIN_SRC})

## Removed custom output directory; use default CMake build output

add_subdirectory("cpp-httplib")
set_target_properties(httplib PROPERTIES OUTPUT_NAME "$<$<NOT:$<PLATFORM_ID:Windows>>:libhttp>")
set_target_properties(httplib PROPERTIES VERSION "" SOVERSION "")
target_compile_options(httplib PRIVATE -fPIC)

if(UNIX OR MINGW)
  find_package(Threads REQUIRED)
  find_package(OpenSSL 3.0 COMPONENTS SSL Crypto REQUIRED)
  # Link only to what we need. Plugin is loaded into engine that provides QuickJS at runtime.
  target_link_libraries(${PROJECT_NAME} httplib Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
endif()

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
