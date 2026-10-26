# ksec_get/ksec_set…：BYOK 密钥 NVS 存取（OOBE 支撑层）
idf_component_register(SRCS "src/kaleido_secret.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash)
