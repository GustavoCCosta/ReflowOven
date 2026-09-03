# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Same reason as tests/boot, tests/controller and tests/coldstart: reuse the
# application Kconfig so the test runs against the port, the client limit and
# the budgets the firmware ships with, instead of a restatement that can drift.
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reflow_httpwait_tests LANGUAGES C)

set(SRC  ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(CORE ${SRC}/core)
set(FAKE ${CMAKE_CURRENT_SOURCE_DIR}/../controller/src)

# httpd.c exactly as it is flashed, with two substitutions at link time:
#
#   - net_wait_fake.c takes the place of ${SRC}/net/l4.c. l4.c is the file that
#     talks to the network stack's event callbacks, and on a simulated platform
#     no link ever comes up, so every wait would be a real timeout.
#   - temp_fake.c takes the place of ${CORE}/temp.c, shared with tests/controller
#     rather than copied. httpd.c needs the telemetry channel and
#     reflow_cmd_post(), which live in controller.c, and controller.c needs a
#     thermocouple that a simulated SPI bus does not have.
#
# The link module (wifi.c / usb_net.c) is NOT in the image: the wait under test
# is link-agnostic by design, which is the property l4.c exists to provide.
target_sources(app PRIVATE
  src/main.c
  src/net_wait_fake.c
  ${FAKE}/temp_fake.c
  ${SRC}/net/httpd.c
  ${SRC}/net/cmdparse.c
  ${SRC}/net/httpgate.c
  ${SRC}/net/profiles_json.c
  ${SRC}/net/sendbudget.c
  ${SRC}/telemetry_json.c
  ${CORE}/controller.c
  ${CORE}/heater.c
  ${CORE}/pid.c
  ${CORE}/profile.c
  ${CORE}/tempguard.c
)

#
# This is the whole point of this directory: the link is NOT up when the httpd
# thread first runs, and comes up afterwards. It has to be a build-time value,
# because the thread waits from K_THREAD_DEFINE before any test body could set
# anything (see tests/coldstart, and the lesson of RFO-G17).
#
# Two failures, not one: one would be satisfied by a server that retries
# exactly once, which is a different bug with the same shape.
#
target_compile_definitions(app PRIVATE REFLOW_NET_WAIT_FAKE_FAILURES=2)

target_include_directories(app PRIVATE ${SRC} ${CORE} ${SRC}/net ${FAKE}
                                       ${CMAKE_CURRENT_SOURCE_DIR}/src)
