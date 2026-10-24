idf_component_register(
    SRCS "kaleido_app.c" "apps/kaleido_apps_builtin.c"
    INCLUDE_DIRS "include"
    REQUIRES json         # IDF 自带 cJSON
    PRIV_REQUIRES log
)
