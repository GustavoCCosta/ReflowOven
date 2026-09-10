# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Same reason as tests/boot: reuse the application's Kconfig instead of
# restating the CONFIG_REFLOW_* symbols heater.c needs, so the test cannot
# drift away from the defaults the firmware ships with.
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reflow_fatal_tests LANGUAGES C)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

# heater.c and fatal.c, and no control thread: the property under test is that
# the gate goes low on the fatal path itself, with nothing else running. Linking
# controller.c would put a control loop in the image and its stale-duty disarm
# could drive the gate low for reasons that have nothing to do with this patch.
target_sources(app PRIVATE
  ${SRC}/core/heater.c
  ${SRC}/core/fatal.c
)

# The two scenarios need different mains: one is a ztest image, the other is a
# plain application that panics and is judged by the console harness. ztest's
# own k_sys_fatal_error_handler (CONFIG_ZTEST_FATAL_HOOK) is a strong symbol and
# would collide with fatal.c, so the real-fault scenario cannot be a ztest.
if(CONFIG_ZTEST)
  target_sources(app PRIVATE src/main.c)
else()
  target_sources(app PRIVATE src/panic.c)
endif()

target_include_directories(app PRIVATE ${SRC} ${SRC}/core)
