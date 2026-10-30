/*
 * main.c — 灵魂内核 on-target 回归入口：串接三套 Unity 套件
 * （W0-1 起 drive/emotion/ignition；emotion 套件含 kd→ke 链锚）
 */
void drive_tests_main(void);
void emotion_tests_main(void);
void ignition_tests_main(void);
void ignition_w3_tests_main(void);

void app_main(void)
{
	drive_tests_main();
	emotion_tests_main();
	ignition_tests_main();
	ignition_w3_tests_main();
}
