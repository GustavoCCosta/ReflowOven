# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Make this application's snippets/ directory discoverable, so that
# 'west build -S usb-webui' works with no extra flags. SNIPPET_ROOT otherwise
# contains only the zephyr repository - the application directory is *not*
# added automatically. This must come before find_package(Zephyr), which is
# where snippets are processed.
list(APPEND SNIPPET_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reflow_oven LANGUAGES C)

# Control core: always present. telemetry_json.c is here because the shell, the
# HTTP server and any host tool must all format the same object identically.
target_sources(app PRIVATE
  src/main.c
  src/telemetry_json.c
  src/core/controller.c
  src/core/heater.c
  src/core/pid.c
  src/core/profile.c
  src/core/temp.c
  src/core/tempguard.c
)

# Optional features. Each one is a leaf: nothing in the core references them.
target_sources_ifdef(CONFIG_REFLOW_UI_DISPLAY app PRIVATE src/ui/display_ui.c)
target_sources_ifdef(CONFIG_REFLOW_UI_INPUT   app PRIVATE src/ui/input_ui.c)
target_sources_ifdef(CONFIG_REFLOW_SHELL      app PRIVATE src/shell_cmds.c)

# Web UI: the server plus exactly one link implementation.
target_sources_ifdef(CONFIG_REFLOW_NET           app PRIVATE src/net/httpd.c src/net/cmdparse.c
                                                            src/net/httpgate.c src/net/l4.c
                                                            src/net/profiles_json.c
                                                            src/net/sendbudget.c)
target_sources_ifdef(CONFIG_REFLOW_LINK_WIFI     app PRIVATE src/net/wifi.c)
target_sources_ifdef(CONFIG_REFLOW_LINK_USB_ECM  app PRIVATE src/net/usb_net.c)

target_include_directories(app PRIVATE src)
