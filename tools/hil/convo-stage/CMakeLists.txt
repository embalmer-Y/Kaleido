# V-45 舞台×语音联动 HIL：dsi-lvgl 显示链 + voice-loop-p4 语音链 同应用
cmake_minimum_required(VERSION 3.16)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components")
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(convo_stage)
