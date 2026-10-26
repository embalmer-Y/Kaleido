# 模块布局（V-43）：
#   voice_loop_main.c  菜单轮编排/遥测；provision.c  OOBE Web 配置页
#   clause_pump.c      子句泵（LLM 句读→队列→常驻 feeder→TTS）
#   convo_fsm.c        免按键会话状态机 + 自驱回归
#   wakenet_test.c     WakeNet 引擎（唤醒经 on_wake 通知 FSM）
# llm_stream.c 已升格为 components/kaleido_speech/src/sp_llm.c（ks_llm_*）
idf_component_register(SRCS "voice_loop_main.c" "audio_io.c" "wakenet_test.c"
                              "provision.c" "clause_pump.c" "convo_fsm.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "test_prompt.pcm"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_client esp_http_server esp_timer
                                     esp_codec_dev espressif__cjson
                                     esp_driver_i2s esp_driver_i2c kaleido_speech kaleido_secret fatfs sdmmc espressif__esp-sr)
