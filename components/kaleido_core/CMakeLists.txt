# KF 应用框架：事件环 + 调试模式 + 心跳 + 崩溃取证（kaleido_core.h 设计说明）
idf_component_register(SRCS "src/kaleido_core.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_timer esp_system log)
