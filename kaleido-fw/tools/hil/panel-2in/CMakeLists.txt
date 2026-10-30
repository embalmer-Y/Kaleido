# panel-2in — 2 寸 AMOLED 460×460 CO5300（MIPI-DSI 1 lane）点屏 HIL
# design-表达层重做v2 Phase 1：独立点屏（色条/移动块/FPS），无 LVGL。
# 屏到货即可 idf.py build flash 验证。
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(panel_2in)
