# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Reuse the application's own Kconfig instead of restating the CONFIG_REFLOW_*
# symbols heater.c needs. Restating them would let the test drift away from the
# defaults the firmware actually ships with, which is the opposite of what this
# suite is for. The application Kconfig ends with source "Kconfig.zephyr", so it
# is a complete root on its own.
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reflow_boot_tests LANGUAGES C)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

# heater.c and nothing else: the point is that the SSR pin is safe with no
# control thread in the image at all. Linking controller.c would configure the
# pin from reflow_heater_init() and hide exactly what is being measured.
target_sources(app PRIVATE
  src/main.c
  ${SRC}/core/heater.c
)
target_include_directories(app PRIVATE ${SRC} ${SRC}/core)
