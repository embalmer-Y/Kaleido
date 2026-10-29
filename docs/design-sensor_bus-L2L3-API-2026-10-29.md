# sensor_bus L2/L3 API 契约（W6b 实现直通车；2026-10-29 设计会话）

> plan §5R.7 W6b v2.2 与 research/31 的落地契约：本文档只定**类型、
> 函数签名、语义与测试锚**——下次会话照此实现，不再设计。纯函数
> host-first（与 kex_face 同模式），板上只加"注册表+定时器+消费者
> 分发"薄壳。判据对应 V-48f v2.2 增补①-⑤。

## 1. 类型（components/kaleido_sensor_bus/include/ksb.h）

```c
/* 实体元数据（注册时静态填入，运行期只读） */
struct ksb_desc {
	const char *id;		/* "imu.accel.x" 点分命名空间 */
	const char *unit;	/* "m/s2"（遥测/LLM 语义） */
	uint8_t privacy;	/* 0=public 1=sensitive 2=private */
	uint8_t tier;		/* 反射挂点位图 bit0=L1 可挂 */
	float deadband;		/* L2 死区（量纲=unit 域） */
	float hysteresis;	/* L2 迟回半宽（重挂门槛=deadband+hyst） */
	uint8_t cap;		/* bit0=hw_batch(LRO 位，W6d 用) */
};

/* L2+L3 per-entity 运行态（调用方持有数组；ksb 零动态分配） */
struct ksb_state {
	/* L2 网关 */
	float latched;		/* 上次上报值（死区比较基准） */
	bool dir_up;		/* 当前变化方向（迟回方向感） */
	uint32_t last_emit_ms;
	/* L3 预测器 */
	float ewma;		/* α 由 half_life 定 */
	float ewma_var;		/* 残差方差估计（precision=1/√var） */
	/* GRO 段合并（活跃段） */
	float seg_min, seg_max, seg_sum, seg_last;
	uint16_t seg_n;
	uint32_t seg_t0_ms, seg_t1_ms;
	int8_t seg_trend;	/* 段首尾斜率符号 */
	/* 风暴折叠 */
	uint16_t ev_budget_used;
};

/* 上行事件（层间唯一货币；原始流永不上行） */
struct ksb_event {
	const struct ksb_desc *desc;
	float value;		/* 本值 */
	float delta;		/* 相对 latched 的增量（死区编码语义） */
	float surprise;		/* |x−pred|×precision */
	uint32_t t_ms;
	uint8_t kind;		/* bit0=死区跨越 bit1=surprise 越τ
				   bit2=storm 摘要 bit3=快照刷新 */
};

/* GRO 段摘要（L3 周期冲刷产出；attention top-K 的输入） */
struct ksb_segment {
	const struct ksb_desc *desc;
	float t_first, t_last, min, max, mean, last;	/* 值域统计 */
	uint32_t t0_ms, t1_ms;
	uint16_t n;
	int8_t trend;		/* -1/0/+1 */
	uint8_t anomaly;	/* surprise 越 τ 计数 */
	float surprise_max;
};
```

## 2. 函数（全部纯函数，显式时间）

```c
/* L2 脑干网关：一次采样进，是否上行+编码什么。
 * arousal ∈[0,1]：唤醒度精度加权——互动中 deadband×(1−0.5a)，
 * 安静时 ×1.0（V-48f ⑤双向可测的落点） */
bool ksb_l2(struct ksb_state *s, const struct ksb_desc *d, float x,
	    uint32_t now_ms, float arousal, struct ksb_event *out);

/* L3 小脑：EWMA 更新+surprise 门+段合并。仅当 L2 放行才调（省算） */
bool ksb_l3(struct ksb_state *s, const struct ksb_desc *d, float x,
	    uint32_t now_ms, struct ksb_event *out);

/* 段冲刷（1s 定时；静止段也冲——快照刷新负向判据 V-48f ①） */
void ksb_flush(struct ksb_state *s, const struct ksb_desc *d,
	       uint32_t now_ms, struct ksb_segment *out /*可 NULL*/);

/* 风暴折叠：每实体每 tick 预算 B；超限后本 tick 余量折叠为一条
 * storm 摘要事件（值=均值，kind|=bit2） */
bool ksb_storm_tick(struct ksb_state *s, const struct ksb_desc *d,
		    uint32_t now_ms, struct ksb_event *out);

/* attention top-K：按 surprise_max×trend 权重挑 K 条段摘要
 * （上下文注入器消费，≤500tok 纪律在调用方截断） */
int ksb_topk(const struct ksb_segment *segs, int n, int K,
	     struct ksb_segment *out);
```

## 3. 参数与测试锚（host）

| 参数 | 值 | 锚 |
|---|---|---|
| EWMA half_life | 2s | 阶跃后 2s 处 ewma=初值+0.5Δ（±1%） |
| surprise τ | 3.0 | 正弦稳态（预测命中）surprise<1 恒不越；阶跃必越 |
| deadband 默认 | 实体自定 | 稳态振幅<deadband → 0 上行（V-48f ②压缩有效性） |
| 迟回 | deadband+0.5×deadband | 边界抖动激励（±db 微噪）→ 0 上行 |
| arousal 调制 | ×(1−0.5a) | 同噪声流 a=1 vs a=0 上行数差 >2×（V-48f ⑤） |
| storm 预算 | 32/tick | 1kHz 激励 → 折叠为 1 条 storm+其余丢弃（V-48f ③） |

## 4. 板上薄壳（实现时唯一新增的非纯函数部分）

注册表（静态数组 ≤16 实体）+ 100ms L2/L3 定时器（挂现有 esp_timer
或 soul tick 分频）+ 消费分发（ke novelty 积分/ignition 触发/prompt
注入器）。W6d 配件框架接入点=注册表行+`source` 字段，L2/L3 不感知。

## 5. 明确不做（防实现蔓延）

- 不做环形缓冲原始流存储（层间只传表示）；
- 不做多实体批处理优化（16 实体×100ms 纯算 <1ms，够）；
- L1 反射挂点只留 tier 位与回调位（实装随 W6c claw_cap）；
- 隐私过滤在消费分发层做（ksb 核心不判隐私，保持纯函数）。
