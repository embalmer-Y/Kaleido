/*
 * main.c — 灵魂内核 on-target 回归入口：串接两套 Unity 套件
 */
void emotion_tests_main(void);
void ignition_tests_main(void);

void app_main(void)
{
	emotion_tests_main();
	ignition_tests_main();
}
