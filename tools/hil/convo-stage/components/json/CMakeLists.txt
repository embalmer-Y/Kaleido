# 垫片组件：esp_emote_gen_player 的 CMakeLists 写死 REQUIRES json，
# 其源码实际用的是 cJSON（espressif/cjson）。本组件以 "json" 之名
# 转发 cjson 的头文件路径，避免魔改 managed_components。
idf_component_register(INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
                       REQUIRES cjson)
