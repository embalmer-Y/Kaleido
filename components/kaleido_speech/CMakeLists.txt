idf_component_register(SRCS "src/sp_dashscope.c" "src/sp_minimax.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_websocket_client espressif__cjson esp_http_client
                       PRIV_REQUIRES esp_timer esp_driver_uart esp-tls)
