idf_component_register(
    SRCS "kaleido_app.c" "apps/kaleido_apps_builtin.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson   # IDF v6 起 cJSON 来自组件仓库（espressif/cjson）
    PRIV_REQUIRES log
)
