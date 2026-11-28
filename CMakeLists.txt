cmake_minimum_required(VERSION 3.16)
project(arqsim VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)

file(GLOB_RECURSE ARQ_LIB_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/core/*.cpp
  ${CMAKE_SOURCE_DIR}/src/net/*.cpp
  ${CMAKE_SOURCE_DIR}/src/phy/*.cpp
  ${CMAKE_SOURCE_DIR}/src/channel/*.cpp
  ${CMAKE_SOURCE_DIR}/src/arq/*.cpp
  ${CMAKE_SOURCE_DIR}/src/experiment/*.cpp)

add_library(arqcore STATIC ${ARQ_LIB_SOURCES})
target_include_directories(arqcore PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(arqcore PUBLIC Threads::Threads)
if(WIN32)
  # ws2_32: Winsock UDP sockets, winmm: 1 ms timer resolution
  target_link_libraries(arqcore PUBLIC ws2_32 winmm)
endif()

file(GLOB CLI_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/cli/*.cpp)
add_executable(arqsim ${CLI_SOURCES})
target_link_libraries(arqsim PRIVATE arqcore)
# Single source of truth for the version: `arqsim version` prints project(VERSION).
target_compile_definitions(arqsim PRIVATE ARQSIM_VERSION="${PROJECT_VERSION}")
if(MINGW)
  target_link_options(arqsim PRIVATE -static)
endif()

enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
if(TEST_SOURCES)
  add_executable(arqsim_tests ${TEST_SOURCES})
  target_link_libraries(arqsim_tests PRIVATE arqcore)
  if(MINGW)
    target_link_options(arqsim_tests PRIVATE -static)
  endif()
  add_test(NAME unit_and_integration COMMAND arqsim_tests)
endif()
