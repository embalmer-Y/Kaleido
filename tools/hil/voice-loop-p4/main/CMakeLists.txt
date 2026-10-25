idf_component_register(SRCS "voice_loop_main.c" "audio_io.c" "llm_stream.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "test_prompt.pcm"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_client esp_timer
                                     esp_codec_dev espressif__cjson
                                     esp_driver_i2s esp_driver_i2c kaleido_speech fatfs sdmmc)
