idf_component_register(SRCS "main.c" "lottie_bench.c" "lottie_stage.c" "bg_jpeg.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "assets/test_lottie.json" "assets/bg800x1280.jpg"
                    PRIV_REQUIRES esp_driver_jpeg esp_timer esp_lcd esp_driver_gpio esp_driver_i2c esp_hw_support esp_lcd_jd9366 esp_lcd_touch_jd9366)
