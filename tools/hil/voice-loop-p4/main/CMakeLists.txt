# llm_stream.c 已升格为 components/kaleido_speech/src/sp_llm.c（ks_llm_*）
idf_component_register(SRCS "voice_loop_main.c" "audio_io.c" "wakenet_test.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "test_prompt.pcm"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_client esp_timer
                                     esp_codec_dev espressif__cjson
                                     esp_driver_i2s esp_driver_i2c kaleido_speech fatfs sdmmc espressif__esp-sr)
