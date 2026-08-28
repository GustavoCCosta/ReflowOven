# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Same reason as tests/boot: reuse the application's Kconfig instead of
# restating the CONFIG_REFLOW_* symbols the control core reads. The test then
# runs against the periods, gains and cut-out the firmware actually ships with.
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reflow_controller_tests LANGUAGES C)

set(SRC  ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(CORE ${SRC}/core)

# The whole control path, shipped as-is, with one substitution: temp_fake.c
# takes the place of ${CORE}/temp.c. That file is the only one that talks to the
# MAX6675, and on a simulated platform there is no MAX6675 behind the emulated
# SPI bus - reflow_temp_init() fails, the controller latches FAULT_SENSOR and no
# run ever starts. Swapping it at link time keeps controller.c, heater.c, pid.c,
# profile.c and tempguard.c exactly as they are flashed.
target_sources(app PRIVATE
  src/main.c
  src/temp_fake.c
  ${CORE}/controller.c
  ${CORE}/heater.c
  ${CORE}/pid.c
  ${CORE}/profile.c
  ${CORE}/tempguard.c
)
target_include_directories(app PRIVATE ${SRC} ${CORE})
