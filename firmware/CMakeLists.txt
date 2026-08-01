cmake_minimum_required(VERSION 3.20.0)

# Custom bindings (asix,ax58100) live alongside the app.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ethercat_servo_node)

target_sources(app PRIVATE
  src/main.c
  src/esc.c
)
