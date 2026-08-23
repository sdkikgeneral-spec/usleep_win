// SPDX-License-Identifier: MIT

//============================================================
// usleep_win 単体テスト
//   方針:
//     - 「どの待機経路を通ったか」は usleep_stats_t のカウンタ差分で証明する。
//       設定値と入力値だけでは実装の分岐を踏めているとは限らない（空振りテスト）。
//     - 時間のアサートは上限だけにしない。理論下限（要求待機時間）を必ず置く。
//     - 環境ジッタは中央値・p95 で吸収し、単発の最大値ではフェイルさせない。
//     - 統計はスレッドローカル。必ず同一スレッドで reset -> 待機 -> get する。
//============================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#include "../include/usleep_win.h"

static int g_failures = 0;

// ---- アサート補助 ----
static void assert_true(bool cond, const char* msg)
{
	if (!cond)
	{
		std::fprintf(stderr, "[FAIL] %s\n", msg);
		g_failures++;
	}
}

// 失敗メッセージには実測値を必ず含める（再現できない失敗は価値が低い）
static void assert_ge_u64(uint64_t actual, uint64_t lo, const char* what)
{
	if (!(actual >= lo))
	{
		std::fprintf(stderr, "[FAIL] %s: actual=%llu < lower bound %llu\n",
			what, (unsigned long long)actual, (unsigned long long)lo);
		g_failures++;
	}
}

static void assert_le_u64(uint64_t actual, uint64_t hi, const char* what)
{
	if (!(actual <= hi))
	{
		std::fprintf(stderr, "[FAIL] %s: actual=%llu > upper bound %llu\n",
			what, (unsigned long long)actual, (unsigned long long)hi);
		g_failures++;
	}
}

static void assert_eq_u64(uint64_t actual, uint64_t expect, const char* what)
{
	if (actual != expect)
	{
		std::fprintf(stderr, "[FAIL] %s: actual=%llu, expected=%llu\n",
			what, (unsigned long long)actual, (unsigned long long)expect);
		g_failures++;
	}
}

static void assert_ret(int actual, int expect, const char* what)
{
	if (actual != expect)
	{
		std::fprintf(stderr, "[FAIL] %s: returned %d, expected %d\n", what, actual, expect);
		g_failures++;
	}
}

// ---- 統計スナップショット差分 ----
struct StatDiff
{
	uint64_t spin_relax, yield_switch, yield_sleep0, yield_sleep1, timer_uses;
	uint64_t total() const { return spin_relax + yield_switch + yield_sleep0 + yield_sleep1 + timer_uses; }
};

static usleep_stats_t snap()
{
	usleep_stats_t s{};
	usleep_get_stats(&s);
	return s;
}

static StatDiff diff(const usleep_stats_t& a, const usleep_stats_t& b)
{
	StatDiff d;
	d.spin_relax   = b.spin_relax   - a.spin_relax;
	d.yield_switch = b.yield_switch - a.yield_switch;
	d.yield_sleep0 = b.yield_sleep0 - a.yield_sleep0;
	d.yield_sleep1 = b.yield_sleep1 - a.yield_sleep1;
	d.timer_uses   = b.waitable_timer_uses - a.waitable_timer_uses;
	return d;
}

static void print_diff(const char* tag, const StatDiff& d)
{
	std::printf("    [stats] %-26s relax=%llu switch=%llu sleep0=%llu sleep1=%llu timer=%llu\n",
		tag,
		(unsigned long long)d.spin_relax,
		(unsigned long long)d.yield_switch,
		(unsigned long long)d.yield_sleep0,
		(unsigned long long)d.yield_sleep1,
		(unsigned long long)d.timer_uses);
}

// ---- 統計量 ----
static uint64_t pct(std::vector<uint64_t> v, double p)
{
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	size_t idx = (size_t)(p * (double)(v.size() - 1) + 0.5);
	if (idx >= v.size()) idx = v.size() - 1;
	return v[idx];
}

struct Samples
{
	uint64_t mn, med, p95, mx;
};

static Samples measure(uint64_t usec, int n)
{
	std::vector<uint64_t> v;
	v.reserve((size_t)n);
	for (int i = 0; i < n; i++)
	{
		uint64_t t0 = usleep_now_steady_us();
		usleep_win(usec);
		v.push_back(usleep_now_steady_us() - t0);
	}
	Samples s;
	s.mn  = pct(v, 0.00);
	s.med = pct(v, 0.50);
	s.p95 = pct(v, 0.95);
	s.mx  = pct(v, 1.00);
	return s;
}

static void print_samples(const char* tag, const Samples& s)
{
	std::printf("    %-20s min=%llu p50=%llu p95=%llu max=%llu (us)\n", tag,
		(unsigned long long)s.mn, (unsigned long long)s.med,
		(unsigned long long)s.p95, (unsigned long long)s.mx);
}

//============================================================
// 1. 待機経路の証明（統計カウンタ差分）
//   実装の分岐（src/usleep_ex.cpp: do_sleep_us）:
//     usec == 0                            -> SwitchToThread
//     usec >= timer_first_us               -> WaitableTimer
//     can_hr && usec > prefer_spin_below   -> WaitableTimer
//     LOW_POWER                            -> spin(+Sleep(1))
//     else                                 -> spin(+yield_policy)
//   kProfileThresholds: BALANCED{2000,200} STRICT{1500,500} LOW_POWER{1000,0}
//   HR タイマ可用性は probe_hrtimer_support() でプロセス初回に確定し以後不変。
//   => スピン／イールド経路を踏むには prefer_spin_below 以下の待機長が必須。
//   さらに spin_with_yield_until_us() は remain > spin_last_us のときしか
//   イールドしないので、spin_last_us を待機長より小さくする必要がある。
//============================================================
static void test_paths()
{
	std::puts("[TEST] wait-path proof via stats counters...");

	// --- usec == 0 の特別扱い: SwitchToThread を 1 回だけ ---
	{
		usleep_set_profile(USLP_BALANCED);
		usleep_reset_stats();
		usleep_stats_t a = snap();
		usleep_win(0);
		StatDiff d = diff(a, snap());
		print_diff("usleep_win(0)", d);
		assert_eq_u64(d.yield_switch, 1, "usleep_win(0) must take the SwitchToThread path exactly once");
		assert_eq_u64(d.timer_uses,   0, "usleep_win(0) must not touch the waitable timer");
		assert_eq_u64(d.spin_relax,   0, "usleep_win(0) must not spin");
	}

	// --- nsleep_win(1000ns 未満) は usec==0 に丸められる ---
	{
		usleep_stats_t a = snap();
		nsleep_win(999);
		StatDiff d = diff(a, snap());
		print_diff("nsleep_win(999ns)", d);
		assert_eq_u64(d.yield_switch, 1, "nsleep_win(999) truncates to 0us -> SwitchToThread path");
		assert_eq_u64(d.timer_uses,   0, "nsleep_win(999) must not use the timer");
	}
	{
		// 1us は qpc_now_us() の µs 切り捨てにより 1 回目の判定で満了することがあるので、
		// 「スピンした」の証明は 1 回ではなく回数をまとめて見る。
		usleep_stats_t a = snap();
		for (int i = 0; i < 100; i++) nsleep_win(1000); // ちょうど 1us
		StatDiff d = diff(a, snap());
		print_diff("100 x nsleep_win(1000ns)", d);
		assert_eq_u64(d.yield_switch, 0, "nsleep_win(1000) is 1us, not the usec==0 path");
		assert_ge_u64(d.spin_relax,   1, "nsleep_win(1000) must go through the spin-wait loop");
		assert_eq_u64(d.timer_uses,   0, "1us must not use the waitable timer");
	}

	// --- 旧テストが空振りだった条件を回帰ガードとして固定する ---
	// BALANCED + spin_last_us=200 + usleep_win(1):
	//   1 >= 2000 は偽、1 > 200 も偽 -> spin_with_yield_until_us(target, 200, SLEEP0)
	//   remain(<=1us) <= spin_last_us(200) なので常に cpu_relax。Sleep(0) は 0 回。
	{
		usleep_set_profile(USLP_BALANCED);
		usleep_set_yield_policy(USLP_YIELD_SLEEP0);
		usleep_set_spin_last_us(200);
		usleep_stats_t a = snap();
		for (int i = 0; i < 2000; i++) usleep_win(1);
		StatDiff d = diff(a, snap());
		print_diff("BALANCED spin=200 x 1us", d);
		assert_eq_u64(d.yield_sleep0, 0,
			"REGRESSION GUARD: BALANCED+spin_last_us=200+usleep_win(1) never reaches Sleep(0) "
			"(remain <= spin_last_us). A test claiming to verify Sleep(0) that way is vacuous.");
		assert_eq_u64(d.timer_uses,  0,    "1us must not use the waitable timer under BALANCED");
		assert_ge_u64(d.spin_relax,  2000, "each 1us wait must spin at least once");
	}

	// --- Sleep(0) イールド経路を実際に踏ませる ---
	// spin_last_us=0 にすると remain > spin_last_us が常に真になり、64 回に 1 回
	// Sleep(0) が発行される。usec=200 は prefer_spin_below(200) を超えないので
	// タイマ経路には落ちない。
	{
		usleep_set_profile(USLP_BALANCED);
		usleep_set_yield_policy(USLP_YIELD_SLEEP0);
		usleep_set_spin_last_us(0);
		usleep_stats_t a = snap();
		uint64_t t0 = usleep_now_steady_us();
		usleep_win(200);
		uint64_t dt = usleep_now_steady_us() - t0;
		StatDiff d = diff(a, snap());
		print_diff("BALANCED spin=0 SLEEP0", d);
		assert_ge_u64(d.yield_sleep0, 1, "Sleep(0) yield path must be taken at least once");
		assert_eq_u64(d.timer_uses,   0, "200us == prefer_spin_below must stay on the spin path");
		assert_ge_u64(dt, 190, "usleep_win(200) must wait at least ~200us");
	}

	// --- SwitchToThread イールド経路（STRICT の既定方針） ---
	{
		usleep_set_profile(USLP_STRICT); // yield_policy = SWITCH_THREAD になる
		usleep_set_spin_last_us(0);      // profile の後に呼ぶこと（逆順だと消える）
		usleep_stats_t a = snap();
		uint64_t t0 = usleep_now_steady_us();
		usleep_win(500);                 // 500 > prefer_spin_below(500) は偽 -> スピン経路
		uint64_t dt = usleep_now_steady_us() - t0;
		StatDiff d = diff(a, snap());
		print_diff("STRICT spin=0 SWITCH", d);
		assert_ge_u64(d.yield_switch, 1, "SwitchToThread yield path must be taken at least once");
		assert_eq_u64(d.timer_uses,   0, "500us must not reach the timer path under STRICT");
		assert_ge_u64(dt, 480, "usleep_win(500) must wait at least ~500us");
	}

	// --- Sleep(1) イールド経路 ---
	// 注意: Sleep(1) は分解能次第で 1〜15ms 眠るためオーバーシュートする。
	//       ここは「経路を踏んだこと」と「下限」だけを見る。
	{
		usleep_set_profile(USLP_BALANCED);
		usleep_set_yield_policy(USLP_YIELD_SLEEP1);
		usleep_set_spin_last_us(0);
		usleep_stats_t a = snap();
		uint64_t t0 = usleep_now_steady_us();
		usleep_win(200);
		uint64_t dt = usleep_now_steady_us() - t0;
		StatDiff d = diff(a, snap());
		print_diff("BALANCED spin=0 SLEEP1", d);
		assert_ge_u64(d.yield_sleep1, 1, "Sleep(1) yield path must be taken at least once");
		assert_eq_u64(d.timer_uses,   0, "200us must not reach the timer path");
		assert_ge_u64(dt, 190, "usleep_win(200) must wait at least ~200us");
	}

	// --- YIELD_NONE は純スピン（イールド系カウンタが一切増えない） ---
	{
		usleep_set_profile(USLP_BALANCED);
		usleep_set_yield_policy(USLP_YIELD_NONE);
		usleep_set_spin_last_us(0);
		usleep_stats_t a = snap();
		usleep_win(200);
		StatDiff d = diff(a, snap());
		print_diff("BALANCED spin=0 NONE", d);
		assert_eq_u64(d.yield_sleep0, 0, "YIELD_NONE must not call Sleep(0)");
		assert_eq_u64(d.yield_sleep1, 0, "YIELD_NONE must not call Sleep(1)");
		assert_eq_u64(d.yield_switch, 0, "YIELD_NONE must not call SwitchToThread");
		assert_ge_u64(d.spin_relax,   1, "YIELD_NONE must spin");
	}

	// --- WaitableTimer 経路（usec >= timer_first_us なら HR 可用性に依らず成立） ---
	{
		usleep_set_profile(USLP_BALANCED); // spin_last_us=250, timer_first_us=2000
		usleep_stats_t a = snap();
		uint64_t t0 = usleep_now_steady_us();
		usleep_win(2000);
		uint64_t dt = usleep_now_steady_us() - t0;
		StatDiff d = diff(a, snap());
		print_diff("BALANCED 2000us", d);
		// タイマ生成に失敗する環境では Sleep(ms) フォールバック（yield_sleep1）に落ちる
		assert_true(d.timer_uses >= 1 || d.yield_sleep1 >= 1,
			"2000us must take the coarse-wait path (waitable timer or Sleep(ms) fallback)");
		assert_ge_u64(dt, 1900, "usleep_win(2000) must wait at least ~2ms");
	}

	// --- HR タイマ可用性の判定と prefer_spin_below 境界 ---
	{
		usleep_set_profile(USLP_BALANCED);
		usleep_stats_t a = snap();
		usleep_win(201); // 201 > prefer_spin_below(200)
		StatDiff d = diff(a, snap());
		print_diff("BALANCED 201us", d);
		const bool hr = (d.timer_uses >= 1);
		std::printf("    high-resolution waitable timer: %s\n", hr ? "available" : "not available");
		if (hr)
		{
			// HR ありなら 200us / 201us の境界が実装どおりに効くこと
			usleep_stats_t b = snap();
			usleep_win(200);
			StatDiff e = diff(b, snap());
			assert_eq_u64(e.timer_uses, 0, "200us is exactly at prefer_spin_below -> spin path");
		}
		else
		{
			assert_eq_u64(d.timer_uses, 0, "without an HR timer, 201us must stay on the spin path");
		}
	}

	// --- LOW_POWER: prefer_spin_below==0、末尾スピンなし ---
	{
		usleep_set_profile(USLP_LOW_POWER); // spin_last_us=0, yield=SLEEP1
		usleep_stats_t a = snap();
		uint64_t t0 = usleep_now_steady_us();
		usleep_win(1000); // >= timer_first_us(1000)
		uint64_t dt = usleep_now_steady_us() - t0;
		StatDiff d = diff(a, snap());
		print_diff("LOW_POWER 1000us", d);
		assert_true(d.timer_uses >= 1 || d.yield_sleep1 >= 1,
			"LOW_POWER 1ms must take the coarse-wait path");
		assert_eq_u64(d.spin_relax, 0, "LOW_POWER must not tail-spin after the coarse wait");
		assert_ge_u64(dt, 800, "LOW_POWER usleep_win(1000) must wait close to 1ms");
	}
	usleep_set_profile(USLP_BALANCED);
}

//============================================================
// 2. 待機時間の統計（下限 + p95 上限）
//    上限だけのアサートは即 return 実装でも通ってしまうため、下限を必ず置く。
//============================================================
static void test_timing()
{
	std::puts("[TEST] wait duration statistics (lower bound + p95)...");
	usleep_set_profile(USLP_BALANCED);

	{
		Samples s = measure(1, 200);
		print_samples("usleep_win(1us)", s);
		assert_ge_u64(s.mn,  1,    "1us must not return before 1us has elapsed");
		assert_le_u64(s.p95, 2000, "1us p95 should stay well under 2ms");
	}
	{
		Samples s = measure(100, 100);
		print_samples("usleep_win(100us)", s);
		assert_ge_u64(s.mn,  100,  "100us must never return early");
		assert_le_u64(s.p95, 5000, "100us p95 should stay under 5ms");
	}
	{
		Samples s = measure(1000, 50);
		print_samples("usleep_win(1ms)", s);
		assert_ge_u64(s.mn,  1000,  "1ms must never return early");
		assert_le_u64(s.p95, 10000, "1ms p95 should stay under 10ms");
	}
	{
		Samples s = measure(20000, 10);
		print_samples("usleep_win(20ms)", s);
		assert_ge_u64(s.mn,  20000, "20ms must never return early");
		assert_le_u64(s.p95, 40000, "20ms p95 should stay under 40ms");
	}
	{
		Samples s = measure(1000000, 1); // 1s は 1 サンプルのみ
		print_samples("usleep_win(1s)", s);
		assert_ge_u64(s.med,  1000000, "1s must not return early");
		assert_le_u64(s.med,  1100000, "1s should not overshoot by more than 100ms");
	}
}

//============================================================
// 3. 締切待機
//============================================================
static void test_deadline()
{
	std::puts("[TEST] deadline scheduling...");
	usleep_set_profile(USLP_BALANCED);

	{
		const uint64_t tick = 1000; // 1ms
		const int N = 50;
		uint64_t start = usleep_now_steady_us();
		uint64_t next  = start;
		std::vector<uint64_t> late;
		late.reserve((size_t)N);
		for (int i = 0; i < N; i++)
		{
			next += tick;
			usleep_until_steady_us(next);
			uint64_t now = usleep_now_steady_us();
			late.push_back(now > next ? (now - next) : 0);
		}
		uint64_t total = usleep_now_steady_us() - start;
		std::printf("    lateness p50=%llu p95=%llu max=%llu us / total=%llu us\n",
			(unsigned long long)pct(late, 0.50), (unsigned long long)pct(late, 0.95),
			(unsigned long long)pct(late, 1.00), (unsigned long long)total);
		// 下限: 締切ベースなので N*tick 未満で終わってはいけない（即 return 検出）
		assert_ge_u64(total, (uint64_t)N * tick, "deadline loop finished before N*tick elapsed");
		// ドリフトは累積しない（total は N*tick + 最後の遅れ程度に収まる）
		assert_le_u64(total, (uint64_t)N * tick + 30000, "deadline loop drifted cumulatively");
		assert_le_u64(pct(late, 0.95), 5000, "deadline lateness p95 too large");
	}

	// 過去の時刻: 即 return し、待機経路には一切入らない
	{
		usleep_reset_stats();
		usleep_stats_t a = snap();
		uint64_t t0 = usleep_now_steady_us();
		usleep_until_steady_us(t0 > 1000000 ? t0 - 1000000 : 0);
		uint64_t dt = usleep_now_steady_us() - t0;
		StatDiff d = diff(a, snap());
		print_diff("until_steady_us(past)", d);
		assert_le_u64(dt, 1000, "past deadline must return immediately");
		assert_eq_u64(d.total(), 0, "past deadline must not enter any wait path");
	}

	// target == now も即 return（usec==0 の SwitchToThread 経路にも入らない）
	{
		usleep_stats_t a = snap();
		usleep_until_steady_us(usleep_now_steady_us());
		StatDiff d = diff(a, snap());
		assert_eq_u64(d.timer_uses, 0, "target==now must not use the timer");
	}
}

//============================================================
// 4. Sleep(0) による譲りが実際に効くこと（経路 + 効果の両方）
//============================================================
static void test_yield_effect()
{
	std::puts("[TEST] Sleep(0) yield actually lets a peer thread run...");

	std::atomic<bool> stop{false};
	std::atomic<int>  ticks{0};
	std::thread worker([&]
	{
		while (!stop.load(std::memory_order_relaxed))
			ticks.fetch_add(1, std::memory_order_relaxed);
	});

	// Sleep(0) 経路に確実に入る設定（spin_last_us=0 かつ usec <= prefer_spin_below）
	usleep_set_profile(USLP_BALANCED);
	usleep_set_yield_policy(USLP_YIELD_SLEEP0);
	usleep_set_spin_last_us(0);

	const int loops = 2000;
	usleep_reset_stats();
	usleep_stats_t a = snap();
	uint64_t t0 = usleep_now_steady_us();
	for (int i = 0; i < loops; i++) usleep_win(200);
	uint64_t dt = usleep_now_steady_us() - t0;
	StatDiff d = diff(a, snap());

	stop.store(true, std::memory_order_relaxed);
	worker.join();

	print_diff("2000 x 200us (SLEEP0)", d);
	std::printf("    elapsed=%llu us, worker ticks=%d\n", (unsigned long long)dt, ticks.load());

	// 経路の証明: 1 回の呼び出しにつき最低 1 回は Sleep(0) が出るはず
	assert_ge_u64(d.yield_sleep0, (uint64_t)loops / 2,
		"Sleep(0) must be issued for (nearly) every call");
	assert_eq_u64(d.timer_uses, 0, "this configuration must not use the waitable timer");
	// 効果: ワーカーが進んでいること
	assert_true(ticks.load() > 100, "worker should have run while main yielded");
	// 時間の下限: loops*200us より短く終わってはいけない
	assert_ge_u64(dt, (uint64_t)loops * 200, "total wait time must be at least loops*200us");
}

//============================================================
// 5. setter の検証（不正値は -1、profile は他の設定を上書きする）
//============================================================
static void test_setters()
{
	std::puts("[TEST] setter validation...");
	assert_ret(usleep_set_profile(-1), -1, "set_profile(-1) must fail");
	assert_ret(usleep_set_profile(3),  -1, "set_profile(3) must fail");
	assert_ret(usleep_set_profile(USLP_BALANCED),  0, "set_profile(BALANCED)");
	assert_ret(usleep_set_profile(USLP_STRICT),    0, "set_profile(STRICT)");
	assert_ret(usleep_set_profile(USLP_LOW_POWER), 0, "set_profile(LOW_POWER)");

	assert_ret(usleep_set_yield_policy(-1), -1, "set_yield_policy(-1) must fail");
	assert_ret(usleep_set_yield_policy(4),  -1, "set_yield_policy(4) must fail");
	assert_ret(usleep_set_yield_policy(USLP_YIELD_NONE),  0, "set_yield_policy(NONE)");
	assert_ret(usleep_set_yield_policy(USLP_YIELD_SLEEP1), 0, "set_yield_policy(SLEEP1)");

	assert_ret(usleep_set_power_mode(-1), -1, "set_power_mode(-1) must fail");
	assert_ret(usleep_set_power_mode(3),  -1, "set_power_mode(3) must fail");
	// SetThreadInformation が無い環境でも 0 を返す実装なので、全モードで 0 が期待値。
	// ThreadPowerThrottling の列挙値を誤ると ERROR_INVALID_PARAMETER で -1 になる。
	assert_ret(usleep_set_power_mode(USLP_POWER_ECO),     0, "set_power_mode(ECO)");
	assert_ret(usleep_set_power_mode(USLP_POWER_PERF),    0, "set_power_mode(PERF)");
	assert_ret(usleep_set_power_mode(USLP_POWER_DEFAULT), 0, "set_power_mode(DEFAULT)");

	// set_profile() は spin_last_us / yield_policy を上書きする（順序依存の証明）。
	{
		usleep_set_spin_last_us(0);
		usleep_set_yield_policy(USLP_YIELD_SLEEP0);
		usleep_set_profile(USLP_BALANCED);          // ここで spin_last_us=250 に戻る
		usleep_stats_t a = snap();
		usleep_win(200);
		StatDiff d = diff(a, snap());
		print_diff("set_profile overwrite", d);
		assert_eq_u64(d.yield_sleep0, 0,
			"set_profile(BALANCED) must restore spin_last_us=250 -> no Sleep(0) for a 200us wait");
		assert_ge_u64(d.spin_relax, 1, "the 200us wait must still spin");
	}
	usleep_set_profile(USLP_BALANCED);
}

//============================================================
// 6. タイマー分解能 API（query -> set -> shutdown のラウンドトリップ）
//============================================================
static void test_resolution()
{
	std::puts("[TEST] timer resolution round-trip...");

	assert_ret(usleep_init_timer_resolution(1), 0, "timeBeginPeriod(1) should succeed");
	usleep_shutdown_timer_resolution();
	assert_ret(usleep_init_timer_resolution(0), 0, "init_timer_resolution(0) is a release request");

	unsigned mn = 0, mx = 0, cur0 = 0;
	if (usleep_query_nt_resolution(&mn, &mx, &cur0) != 0)
	{
		std::puts("    NtQueryTimerResolution unavailable - skipping the NT resolution test");
		return;
	}
	std::printf("    NT resolution: min=%u max=%u cur=%u (100ns)\n", mn, mx, cur0);
	assert_true(mx <= mn, "max (finest) resolution value must be <= min (coarsest)");
	assert_true(cur0 >= mx && cur0 <= mn, "current resolution must sit within [max, min]");

	assert_ret(usleep_init_nt_resolution(mx), 0, "NtSetTimerResolution(finest) should succeed");
	unsigned cur1 = 0;
	usleep_query_nt_resolution(nullptr, nullptr, &cur1);
	std::printf("    after init:     cur=%u (100ns)\n", cur1);
	assert_true(cur1 <= cur0, "requesting the finest resolution must not make it coarser");
	assert_eq_u64(cur1, mx, "current resolution should equal the requested finest value");

	usleep_shutdown_nt_resolution();
	unsigned cur2 = 0;
	usleep_query_nt_resolution(nullptr, nullptr, &cur2);
	std::printf("    after shutdown: cur=%u (100ns)\n", cur2);
	// 他プロセスが分解能を握っている場合があるので「戻る or それ以上に粗くなる」を見る
	assert_true(cur2 >= cur1, "shutdown must not keep the system pinned at a finer resolution");

	// 解除済みで二重に呼んでも壊れないこと
	usleep_shutdown_nt_resolution();
	usleep_shutdown_timer_resolution();
}

//============================================================
// 7. スレッド間の独立性（設定・統計はスレッドローカル）
//============================================================
static void test_thread_isolation()
{
	std::puts("[TEST] per-thread config / stats isolation...");

	usleep_set_profile(USLP_BALANCED);
	usleep_set_yield_policy(USLP_YIELD_SLEEP0);
	usleep_set_spin_last_us(0);
	usleep_reset_stats();

	std::atomic<uint64_t> worker_sleep0{0};
	std::atomic<uint64_t> worker_switch{0};
	std::atomic<uint64_t> worker_initial{0};

	std::thread th([&]
	{
		// ワーカーは STRICT（SwitchToThread）に切り替える
		usleep_set_profile(USLP_STRICT);
		usleep_set_spin_last_us(0);

		// 別スレッドの統計は見えない（新しいスレッドのカウンタは 0 から始まる）
		usleep_stats_t s0 = snap();
		worker_initial.store(s0.spin_relax + s0.yield_switch + s0.yield_sleep0
			+ s0.yield_sleep1 + s0.waitable_timer_uses, std::memory_order_relaxed);

		for (int i = 0; i < 200; i++) usleep_win(500);
		usleep_stats_t s1 = snap();
		worker_sleep0.store(s1.yield_sleep0, std::memory_order_relaxed);
		worker_switch.store(s1.yield_switch, std::memory_order_relaxed);
	});

	for (int i = 0; i < 200; i++) usleep_win(200); // メイン: BALANCED + Sleep(0)
	usleep_stats_t m = snap();
	th.join();

	std::printf("    main: sleep0=%llu switch=%llu | worker: sleep0=%llu switch=%llu\n",
		(unsigned long long)m.yield_sleep0, (unsigned long long)m.yield_switch,
		(unsigned long long)worker_sleep0.load(), (unsigned long long)worker_switch.load());

	assert_eq_u64(worker_initial.load(), 0, "stats are thread-local: a fresh thread starts at 0");
	assert_ge_u64(m.yield_sleep0, 100, "main thread must have used the Sleep(0) policy");
	assert_eq_u64(m.yield_switch, 0,   "main thread must not be affected by the worker's STRICT policy");
	assert_ge_u64(worker_switch.load(), 100, "worker thread must have used the SwitchToThread policy");
	assert_eq_u64(worker_sleep0.load(), 0,   "worker thread must not be affected by main's SLEEP0 policy");
}

int main()
{
	std::puts("[TEST] usleep_win test suite");

	test_paths();
	test_timing();
	test_deadline();
	test_yield_effect();
	test_setters();
	test_resolution();
	test_thread_isolation();

	if (g_failures)
	{
		std::printf("[NG] %d assertion(s) failed.\n", g_failures);
		return 1;
	}
	std::puts("[OK] all tests passed.");
	return 0;
}
