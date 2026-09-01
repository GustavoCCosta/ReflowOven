# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Same reason as tests/boot and tests/controller: reuse the application Kconfig
# so the test runs against the periods and the retry interval the firmware ships
# with, instead of a restatement that can drift.
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reflow_coldstart_tests LANGUAGES C)

set(SRC  ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(CORE ${SRC}/core)
set(FAKE ${CMAKE_CURRENT_SOURCE_DIR}/../controller/src)

# The same image as tests/controller, and the same fake - shared rather than
# copied, so the two suites cannot drift. What differs is one build-time value.
target_sources(app PRIVATE
  src/main.c
  ${FAKE}/temp_fake.c
  ${CORE}/controller.c
  ${CORE}/heater.c
  ${CORE}/pid.c
  ${CORE}/profile.c
  ${CORE}/tempguard.c
)

#
# This is the whole point of this directory: the front-end is missing when the
# control thread first runs. It has to be a build-time value, because the thread
# initialises before any test body could set anything (RFO-B39, and the lesson
# of RFO-G17 about knobs that look usable and are not).
#
target_compile_definitions(app PRIVATE REFLOW_TEMP_FAKE_INIT_FAILURES=1)

target_include_directories(app PRIVATE ${SRC} ${CORE} ${FAKE})
