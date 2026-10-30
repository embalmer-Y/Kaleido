# convo-stage（V-45 合并工程；语音模块自 voice-loop-p4 平移）：
#   convo_stage_main.c  轮编排
#   soul.c/soul_think.c 灵魂 limb + W3 思维轮编排
#   mem.c               W4 记忆接线（littlefs/情节写回/检索注入）
#   （显示链/表情舞台已于 2026-10-28 整体拆除——表达层待 2 寸屏重做，
#     拆除说明与取回入口见 docs/表达层拆除与重做-2026-10-28.md）
idf_component_register(SRCS "convo_stage_main.c"
                              "audio_io.c" "wakenet_test.c" "provision.c"
                              "clause_pump.c" "convo_fsm.c" "aec_probe.c" "soul.c" "soul_think.c" "mood.c"
                              "mem.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "test_prompt.pcm"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_client esp_http_server esp_timer
                                     esp_codec_dev espressif__cjson
                                     esp_driver_i2s esp_driver_i2c
                                     esp_driver_gpio
                                     esp_hw_support
                                     littlefs
                                     kaleido_speech kaleido_secret
                                     fatfs sdmmc espressif__esp-sr kaleido_core
                                     kaleido_drive kaleido_emotion kaleido_ignition
                                     kaleido_memory)
