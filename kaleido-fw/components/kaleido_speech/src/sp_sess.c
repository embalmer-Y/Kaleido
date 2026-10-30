/*
 * sp_sess.c — provider 会话互斥锁（V-47，见 sp_internal.h）
 * 懒初始化：首次 take 时创建（组件无显式 init 入口）。
 */
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_sess;
static bool s_held;		/* 释放校验：give 必须配对 take */

bool sp_sess_take(uint32_t timeout_ms)
{
	if (!s_sess)
		s_sess = xSemaphoreCreateMutex();
	if (!s_sess)
		return false;
	if (xSemaphoreTake(s_sess, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
		return false;
	s_held = true;
	return true;
}

void sp_sess_give(void)
{
	if (s_held) {
		s_held = false;
		xSemaphoreGive(s_sess);
	}
}
