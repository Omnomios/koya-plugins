cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0048 NEW)

set(PROJECT_NAME "sm-sqlite")

project(${PROJECT_NAME} VERSION 0.0.1 DESCRIPTION "Koya SQLite Module")

set(PROJECT_SOURCE_DIR  "src")
file(GLOB_RECURSE MAIN_SRC "${PROJECT_SOURCE_DIR}/*.cpp")

include_directories(src ../../. ../../sdk)

add_library(${PROJECT_NAME} SHARED ${MAIN_SRC})

find_package(SQLite3 REQUIRED)
target_link_libraries(${PROJECT_NAME} SQLite::SQLite3)

if(UNIX OR MINGW)
  find_package(Threads REQUIRED)
  target_link_libraries(${PROJECT_NAME} Threads::Threads)
endif()

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)


