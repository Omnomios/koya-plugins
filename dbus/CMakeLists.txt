cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0048 NEW)

set(PROJECT_NAME "sm-dbus")

project(${PROJECT_NAME} VERSION 0.0.1 DESCRIPTION "Koya D-Bus Module")

set(PROJECT_SOURCE_DIR  "src")
file(GLOB_RECURSE MAIN_SRC "${PROJECT_SOURCE_DIR}/*.cpp")

include_directories(src ../../. ../../sdk)

add_library(${PROJECT_NAME} SHARED ${MAIN_SRC})

## Removed custom output directory; use default CMake build output

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)
include_directories(${DBUS_INCLUDE_DIRS})
link_directories(${DBUS_LIBRARY_DIRS})
add_definitions(${DBUS_CFLAGS_OTHER})

if(UNIX OR MINGW)
  find_package(Threads REQUIRED)
  target_link_libraries(${PROJECT_NAME} Threads::Threads ${DBUS_LIBRARIES})
endif()

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)

