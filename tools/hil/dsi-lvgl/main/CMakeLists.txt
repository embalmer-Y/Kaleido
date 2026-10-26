# lottie_bench.c（V-35R 基准）已归档不参与编译；bg_jpeg.c 为封存能力，
# 壁纸场景启用时再 EMBED 资产调用（V-36 定稿）
idf_component_register(SRCS "main.c" "lottie_stage.c" "bg_jpeg.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "assets/test_lottie.json"
                    PRIV_REQUIRES esp_driver_jpeg esp_timer esp_lcd esp_driver_gpio esp_driver_i2c esp_hw_support esp_lcd_jd9366 esp_lcd_touch_jd9366)
