cmake_minimum_required(VERSION 3.13)
project(servo_master C)

set(CMAKE_C_STANDARD 99)
add_compile_options(-Wall -Wextra)

# Unit tests build anywhere (Mac or Pi) — no SOEM needed.
add_executable(test_angle test/test_angle.c src/angle.c)
target_link_libraries(test_angle m)
enable_testing()
add_test(NAME angle COMMAND test_angle)

# The master app needs SOEM and Linux raw sockets — Pi only.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "Non-Linux host: building test_angle only (servo_master is Pi-only)")
  return()
endif()

# Prefer a SOEM checkout made by scripts/pi-setup.sh; fall back to
# fetching the pinned tag directly.
set(SOEM_LOCAL "$ENV{HOME}/ethercat/SOEM")
include(FetchContent)
if(EXISTS "${SOEM_LOCAL}/CMakeLists.txt")
  message(STATUS "Using local SOEM at ${SOEM_LOCAL}")
  FetchContent_Declare(soem SOURCE_DIR "${SOEM_LOCAL}")
else()
  FetchContent_Declare(soem
    GIT_REPOSITORY https://github.com/OpenEtherCATsociety/SOEM.git
    GIT_TAG v1.4.0)
endif()
FetchContent_MakeAvailable(soem)

add_executable(servo_master src/main.c src/ecat.c src/angle.c)
# SOEM v1.4.0 uses directory-scoped include_directories, so its headers
# don't propagate through the soem target — add them explicitly.
target_include_directories(servo_master PRIVATE
  ${soem_SOURCE_DIR}/soem
  ${soem_SOURCE_DIR}/osal
  ${soem_SOURCE_DIR}/osal/linux
  ${soem_SOURCE_DIR}/oshw/linux)
target_link_libraries(servo_master soem m pthread rt)
