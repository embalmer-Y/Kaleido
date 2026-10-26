# llm_stream.c 已升格为 components/kaleido_speech/src/sp_llm.c（ks_llm_*）
# provision.c：OOBE Web 配置页（V-42）；kaleido_secret：BYOK 密钥 NVS 层
idf_component_register(SRCS "voice_loop_main.c" "audio_io.c" "wakenet_test.c" "provision.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "test_prompt.pcm"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_client esp_http_server esp_timer
                                     esp_codec_dev espressif__cjson
                                     esp_driver_i2s esp_driver_i2c kaleido_speech kaleido_secret fatfs sdmmc espressif__esp-sr)
