cmake_minimum_required(VERSION 3.16)
# 情感引擎 on-target 回归：main 即组件的 Unity 测试（板恢复后 idf.py flash monitor 跑）
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(emotion-test)
