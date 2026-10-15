# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_STUDIO_LOG)
  zephyr_include_directories(include)
  target_sources(app PRIVATE src/studio_log.c)
endif()
