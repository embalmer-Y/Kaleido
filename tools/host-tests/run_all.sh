#!/bin/bash
# Kaleido 主机回归（评审 A2 要求：四套件全覆盖，无需开发板）
# 依赖：gcc、IDF 源码树（Unity）、组件内 cJSON
set -e
FW="$(cd "$(dirname "$0")/../.." && pwd)"
IDF=${IDF_PATH:-$HOME/esp/esp-idf}
CJSON="$FW/managed_components/espressif__cjson/cJSON"
UNITY="$IDF/components/unity/unity"
CC="gcc -Wall -Wno-unused-function -I$CJSON -I$UNITY/src -I$UNITY/component"
TMP=$(mktemp -d)

run() {  # name main.c extra_src...
	local name=$1 main=$2; shift 2
	$CC -I"$FW/components/kaleido_$name/include" \
		-o "$TMP/$name" "$main" "$@" \
		"$FW/components/kaleido_$name/src/kaleido_$name.c" \
		"$CJSON/cJSON.c" -lm
	"$TMP/$name"
}

run memory "$FW/tools/host-tests/km_host_test.c"
run persona "$FW/tools/host-tests/kp_host_test.c"

# emotion/ignition：Unity 直编（主机 main 包装 unity_runner）
cat > "$TMP/ke_main.c" <<'UEOF'
#include "unity_internals.h"
void emotion_tests_main(void);
void ignition_tests_main(void);
void setUp(void) {}
void tearDown(void) {}
int main(void)
{
	emotion_tests_main();
	ignition_tests_main();
	return Unity.TestFailures ? 1 : 0;
}
UEOF
gcc -Wall -I"$FW/components/kaleido_emotion/include" \
	-I"$FW/components/kaleido_ignition/include" -I$UNITY/src \
	-o "$TMP/soul" "$TMP/ke_main.c" \
	"$FW/components/kaleido_emotion/test/test_kaleido_emotion.c" \
	"$FW/components/kaleido_emotion/src/kaleido_emotion.c" \
	"$FW/components/kaleido_ignition/test/test_kaleido_ignition.c" \
	"$FW/components/kaleido_ignition/src/kaleido_ignition.c" \
	"$UNITY/src/unity.c" -lm
"$TMP/soul"

echo "HOST_REGRESSION_ALL_PASS"
