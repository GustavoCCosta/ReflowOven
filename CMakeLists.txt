# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reflow_oven LANGUAGES C)

# Control core: always present.
target_sources(app PRIVATE
  src/main.c
  src/core/controller.c
  src/core/heater.c
  src/core/pid.c
  src/core/profile.c
  src/core/temp.c
)

# Optional features. Each one is a leaf: nothing in the core references them.
target_sources_ifdef(CONFIG_REFLOW_UI_DISPLAY app PRIVATE src/ui/display_ui.c)
target_sources_ifdef(CONFIG_REFLOW_UI_INPUT   app PRIVATE src/ui/input_ui.c)
target_sources_ifdef(CONFIG_REFLOW_NET        app PRIVATE src/net/wifi.c src/net/httpd.c)
target_sources_ifdef(CONFIG_REFLOW_SHELL      app PRIVATE src/shell_cmds.c)

target_include_directories(app PRIVATE src)
