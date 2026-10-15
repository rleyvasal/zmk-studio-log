# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_STUDIO_LOG)
  zephyr_include_directories(include)
  target_sources(app PRIVATE src/studio_log.c)
  target_sources_ifdef(CONFIG_ZMK_STUDIO_LOG_RESTART_DIAG app PRIVATE src/restart_diag.c)
  target_sources_ifdef(CONFIG_ZMK_STUDIO_LOG_BLACK_BOX app PRIVATE src/black_box.c)
endif()
