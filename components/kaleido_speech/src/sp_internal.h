/*
 * sp_internal.h — kaleido_speech 组件内部共享声明（不对外）
 *
 * 各 provider 实现文件（sp_*.c）之间复用的小件；公开 API 一律走
 * include/kaleido_speech.h，此处出现的东西不许跨组件引用。
 */
#ifndef SP_INTERNAL_H
#define SP_INTERNAL_H

#include <stddef.h>

/*
 * 从 MiniMax JWT（header.payload.sig）自解析 GroupID。
 * 手写 base64url 解码：不引入 JWT 依赖树，只为取一个字段。
 * 返回 0 成功；-1 格式错；-2 GroupID 超过 out 容量（宁报错不出残串）。
 */
int sp_mm_group_id(const char *jwt, char *out, size_t n);

#endif /* SP_INTERNAL_H */
