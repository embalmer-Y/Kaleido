idf_component_register(
    SRCS "soul_bench_main.c" "bench_a.c" "bench_b.c" "bench_c.c" "bench_util.c"
    INCLUDE_DIRS "."
    EMBED_FILES "assets/test_lottie.json" "assets/test460.jpg"
    PRIV_REQUIRES mbedtls pthread esp_timer
)
