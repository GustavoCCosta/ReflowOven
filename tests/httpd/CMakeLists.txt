# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Same reason as tests/httpwait, tests/controller and tests/coldstart: reuse the
# application Kconfig so the test runs against the port, the client limit and
# the budgets the firmware ships with, instead of a restatement that can drift.
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reflow_httpd_tests LANGUAGES C)

set(SRC  ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(CORE ${SRC}/core)
set(FAKE ${CMAKE_CURRENT_SOURCE_DIR}/../controller/src)

# src/net/httpd.c is deliberately NOT here: src/main.c includes it, so that the
# test can reach send_all(), which is static and is where the RFO-B07 fix
# lives. Listing it as well would compile it twice and collide at link.
#
# Everything else is the firmware's own code, plus the two substitutions
# tests/httpwait already established: net_wait_fake.c for l4.c, because no link
# ever comes up on a simulated platform, and temp_fake.c for temp.c, because
# controller.c - which owns the telemetry channel httpd.c reads - needs a
# thermocouple that a simulated SPI bus does not have.
target_sources(app PRIVATE
  src/main.c
  src/socket_fake.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../httpwait/src/net_wait_fake.c
  ${FAKE}/temp_fake.c
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

# The link comes up on the first ask: this suite is not about the wait, and a
# server thread still waiting would be one more thing happening while the
# scenarios run. tests/httpwait is where the wait itself is measured.
target_compile_definitions(app PRIVATE REFLOW_NET_WAIT_FAKE_FAILURES=0)

# Same generated page as the firmware, from the same rule (RFO-B25): this image
# compiles httpd.c, so it needs index_html.h too.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/index_html.cmake)
reflow_generate_index_html(${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_include_directories(app PRIVATE ${SRC} ${CORE} ${SRC}/net ${FAKE}
                                       ${CMAKE_CURRENT_SOURCE_DIR}/src
                                       ${CMAKE_CURRENT_SOURCE_DIR}/../httpwait/src)
