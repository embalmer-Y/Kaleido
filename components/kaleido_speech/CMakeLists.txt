# sp_dashscope/sp_minimax：ASR/TTS provider（TTS 按 Kconfig 二选一编译）
# sp_llm：流式 chat/completions 客户端（含 MiniMax JWT→GroupID 工具）
# mbedtls：esp_crt_bundle（esp-tls 只是传递依赖，esp_driver_uart 从未用到）
idf_component_register(EMBED_FILES "gs_r46.pem"
                       SRCS "src/sp_dashscope.c" "src/sp_minimax.c" "src/sp_llm.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_websocket_client espressif__cjson esp_http_client
                       PRIV_REQUIRES esp_timer mbedtls)
