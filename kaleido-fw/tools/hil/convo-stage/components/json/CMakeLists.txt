# 垫片组件：esp_emote_gen_player 的 CMakeLists 写死 REQUIRES json，
# 其源码实际用的是 cJSON（espressif/cjson）。本组件仅以 "json" 之名
# 公开传递 cjson 依赖（REQUIRES 为公共传播，头文件路径直达 gen_player），
# 不魔改 managed_components、不持符号链接（可移植性，评审#8）。
idf_component_register(REQUIRES cjson)
