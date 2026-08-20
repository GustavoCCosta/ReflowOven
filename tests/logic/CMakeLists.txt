# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reflow_logic_tests LANGUAGES C)

set(SRC  ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(CORE ${SRC}/core)

target_sources(app PRIVATE
  src/main.c
  ${CORE}/pid.c
  ${CORE}/profile.c
  ${SRC}/net/cmdparse.c
  ${SRC}/net/httpgate.c
)
target_include_directories(app PRIVATE ${CORE} ${SRC})
