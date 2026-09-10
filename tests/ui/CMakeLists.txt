# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# O root aponta para o Kconfig DESTA suite, que declara o symbol de cena e
# depois puxa o da aplicacao inteiro - as duas propriedades do RFO-B12 nao
# cabem numa imagem so (ver tests/ui/Kconfig).
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reflow_ui_tests LANGUAGES C)

set(SRC  ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(CORE ${SRC}/core)

# display_ui.c is the subject, and the control core comes along because
# reflow_telemetry_chan is DEFINED in controller.c. Restating the channel here
# would let this suite drift away from the real one - the observer queue depth
# and the publish timeout are exactly what is being measured.
#
# temp_fake.c substitutes ${CORE}/temp.c for the same reason as tests/controller:
# there is no MAX6675 behind an emulated SPI bus.
target_sources(app PRIVATE
  src/main.c
  src/temp_fake.c
  ${SRC}/ui/display_ui.c
  ${CORE}/controller.c
  ${CORE}/heater.c
  ${CORE}/pid.c
  ${CORE}/profile.c
  ${CORE}/tempguard.c
)
target_include_directories(app PRIVATE ${SRC} ${CORE})
