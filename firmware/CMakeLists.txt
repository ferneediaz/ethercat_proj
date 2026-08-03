cmake_minimum_required(VERSION 3.20.0)

# Custom bindings (asix,ax58100) live alongside the app.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ethercat_servo_node)

# SOES lives behind CONFIG_ESC_USE_SOES. Its sources are compiled straight
# into the app rather than as a separate library: it needs our ecat_options.h
# and cc.h on the include path, and a nested library would have to re-export
# both anyway.
if(CONFIG_ESC_USE_SOES)
  set(SOES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/soes)

  # Fetched, not vendored — the same approach master/CMakeLists.txt uses for
  # SOEM. Committing the tree would add ~127 files of demos, drivers and ESI
  # binaries for the three .c files we actually compile.
  #
  # SOURCE_SUBDIR points at a directory that does not exist on purpose: it
  # stops FetchContent from running SOES's own CMakeLists, which builds a
  # standalone library with its own config. We only want the sources.
  include(FetchContent)
  FetchContent_Declare(soes
    GIT_REPOSITORY https://github.com/OpenEtherCATsociety/SOES.git
    # Pinned to the exact commit this port was verified against on hardware
    # (2026-08-02). Tags v3.1.0 and earlier are untested here.
    GIT_TAG 6ef7b9415e936c061ad30626ca00004bdde2ac62
    SOURCE_SUBDIR do-not-configure)
  FetchContent_MakeAvailable(soes)

  set(SOES_SRC ${soes_SOURCE_DIR}/soes)

  target_include_directories(app PRIVATE
    ${SOES_DIR} ${SOES_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/src)

  target_sources(app PRIVATE
    ${SOES_SRC}/esc.c
    ${SOES_SRC}/esc_coe.c
    ${SOES_SRC}/ecat_slv.c
    ${SOES_DIR}/esc_hw.c
    ${SOES_DIR}/soes_app.c
    ${SOES_DIR}/slave_objectlist.c
  )
endif()

target_sources(app PRIVATE
  src/main.c
  src/ax58100.c
  src/diag.c
  src/servo.c
  src/stepper.c
  src/ecat_slave.c
  src/sync0.c
)
