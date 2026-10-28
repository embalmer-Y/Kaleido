# V-45R 表情舞台（V-45 合并工程；语音模块自 voice-loop-p4 平移）：
#   convo_stage_main.c  轮编排 + DSI/LVGL 初始化 + 表达桥接线
#   stage_express.c     kaleido_express(L0) ↔ 舞台/表情桥
#   emote_stage.c       表情舞台（esp_emote_gen_player 猫伴表情包，
#                       RGB565 画布直写合成；thorvg/lottie 版见 git 历史）
idf_component_register(SRCS "convo_stage_main.c" "stage_express.c" "emote_stage.c"
                              "audio_io.c" "wakenet_test.c" "provision.c"
                              "clause_pump.c" "convo_fsm.c" "aec_probe.c" "soul.c" "soul_think.c" "mood.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "test_prompt.pcm"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_client esp_http_server esp_timer
                                     esp_codec_dev espressif__cjson
                                     esp_driver_i2s esp_driver_i2c
                                     esp_lcd esp_driver_gpio esp_driver_jpeg
                                     esp_hw_support
                                     esp_lvgl_port lvgl
                                     esp_lcd_jd9366 esp_lcd_touch_jd9366
                                     kaleido_speech kaleido_secret kaleido_express
                                     fatfs sdmmc espressif__esp-sr kaleido_core
                                     espressif2022__esp_emote_gen_player
                                     kaleido_drive kaleido_emotion kaleido_ignition)
