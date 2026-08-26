// SPDX-License-Identifier: MIT

//============================================================
// ベンチ: ジッタ / CPU 使用率 / 譲り回数 / タイマ使用回数を CSV 出力
//	 使い方(例): bench_usleep_csv.exe [iters] [tick_us] [spin_last_us] [yield_policy] [profile] [label]
//	   iters		 : 反復回数（既定 2000）
//	   tick_us		 : 目標周期[µs]（既定 1000 = 1ms）
//	   spin_last_us  : 最後のスピン時間[µs]（既定 200）
//	   yield_policy  : 0=NONE 1=SWITCH 2=SLEEP0 3=SLEEP1（既定 2=SLEEP0）
//	   profile		 : 0=BALANCED 1=STRICT 2=LOW_POWER（既定 0=BALANCED）
//	   label		 : サマリに出す構成名（既定 "bench"）
//
//	 環境変数:
//	   USLEEP_BENCH_NT_RES_100NS
//		 未設定 … 既定。タイマ分解能に一切触らない（他プロセス任せの実情を測る）
//		 "max"  … NtQueryTimerResolution が返す最も細かい値を明示設定する
//		 数値   … その値[100ns 単位]を NtSetTimerResolution で設定する
//					（例: 5000 = 0.5ms、10000 = 1ms）
//		 測定後は必ず usleep_shutdown_nt_resolution() で解除する。
//		 タイマ分解能はプロセス横断の共有状態であり、WaitableTimer の早着量
//		 ＝末尾スピンの長さ＝CPU% に直接効く。測定条件として必ず記録すること。
//
//	 このファイルは UTF-8 BOM 付きで保存すること（プロジェクト規約）。
//============================================================

//============================================================
// CPU 使用率の定義
//	 分子 = 測定スレッド自身が消費した CPU 時間
//	 分母 = 同区間の実時間（QPC）
//	 → 「論理コア 1 個を 100% とする」定義。単一スレッドのベンチなので上限は 100%。
//
//	 分子は 2 通り出す。両者を併記するのは、片方だけでは誤読するため。
//	   (a) thread_cycle_cpu_pct_of_1core
//		   QueryThreadCycleTime のサイクル差分を、起動時に実測した換算係数で
//		   時間に直したもの。これを主値として読むこと。
//	   (b) thread_cpu_pct_of_1core
//		   GetThreadTimes の kernel+user 差分。参考値。
//
//	 参考としてプロセス全体（全スレッド合算）の値と、それを論理コア数で割った
//	 「システム全体を 100% とする」値も併記する。プロセス値には CRT / ローダ等の
//	 ベンチ以外のスレッドが含まれるため、測りたい量とは一致しない。
//
//	 計器の実装（および GetThreadTimes のティックサンプリング課金がなぜ
//	 この負荷型で桁違いに外すか）は tools/cpu_accounting.h に集約してある。
//	 同じヘッダを tests/test_usleep.cpp が include し、純ビジー待機が論理コア
//	 1 個の 100% 付近を返すことを回帰テストしている（計器が壊れたら落ちる）。
//============================================================

//============================================================
// per-iteration の CPU% を出さない理由
//	 GetThreadTimes の分解能はクロックティックなので、1ms 周期の 1 区間では
//	 0 か 1 ティック分かの二値にしかならない。さらに締切方式では区間長が不均一
//	 （即 return する区間は数 µs）になるため、区間ごとの比率を単純平均すると
//	 分母が極小の区間で桁違いに暴れる。
//	 → CSV の CPU% 列は「ループ開始からの累積」（合計 ÷ 合計）のみとし、
//		区間ごとの瞬時値は出さない。累積列はサイクル計から作る。
//
// 計測を汚さないための約束
//	 - printf は測定ループの外でまとめて行う。ループ内で書くと printf 自身の
//	   CPU 時間が分子に入り、CPU% が跳ね上がる（1ms 周期では数 % 規模の汚染）。
//	 - 記録用バッファはループ前に reserve 済み。ループ内でヒープを触らない。
//	 - それでも QueryThreadCycleTime / QueryPerformanceCounter / usleep_get_stats の
//	   呼び出しコスト自体は分子に入る。これは計測の原理的な下駄であり、
//	   「ベンチの計装込みの CPU%」として読むこと。
//============================================================

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <algorithm>
#include <vector>
#include "../include/usleep_win.h"
#include "cpu_accounting.h"

using uslp_cpu::qpc_now_us;
using uslp_cpu::thread_cycles;

// 大文字小文字を無視した文字列比較（処理系依存の _stricmp を使わない）
static bool eq_nocase(const char* a, const char* b)
{
	for (; *a && *b; ++a, ++b)
	{
		const char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
		const char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
		if (ca != cb) return false;
	}
	return *a == *b;
}

// 昇順ソート済みベクタからパーセンタイルを取る（最近傍順位法）
static uint64_t pct_of_sorted(const std::vector<uint64_t>& v, double p)
{
	if (v.empty()) return 0;
	size_t idx = (size_t)(p * (double)(v.size() - 1) + 0.5);
	if (idx >= v.size()) idx = v.size() - 1;
	return v[idx];
}

// ---- 1 反復あたりの記録（ループ内では printf せず、ここに溜める） ----
struct Sample
{
	uint64_t late_us;	 // 締切からの遅着[µs]（早着は 0 丸め）
	uint64_t wall_d_us;	 // ループ開始からの累積実時間[µs]
	uint64_t cyc_d;		 // ループ開始からの累積サイクル
	uint64_t d_spin;
	uint64_t d_swth;
	uint64_t d_s0;
	uint64_t d_s1;
	uint64_t d_timer;
};

int main(int argc, char** argv)
{
	int			iters		 = (argc > 1) ? atoi(argv[1]) : 2000;
	uint64_t	tick_us		 = (argc > 2) ? strtoull(argv[2], nullptr, 10) : 1000ULL;
	unsigned	spin_last_us = (argc > 3) ? (unsigned)strtoul(argv[3], nullptr, 10) : 200U;
	int			yield_policy = (argc > 4) ? atoi(argv[4]) : USLP_YIELD_SLEEP0;
	int			profile		 = (argc > 5) ? atoi(argv[5]) : USLP_BALANCED;
	const char* label		 = (argc > 6) ? argv[6] : "bench";

	if (iters <= 0)	  iters	  = 1;
	if (tick_us == 0) tick_us = 1;

	LARGE_INTEGER f;
	if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0)
	{
		fprintf(stderr, "QueryPerformanceFrequency failed\n");
		return 1;
	}
	// QPC 周波数の取得と動的 API の解決。周波数はここで一度だけ取り、
	// 測定ループ内では取らない。
	uslp_cpu::init();

	const unsigned ncpu = uslp_cpu::logical_cpu_count();

	// 設定は必ず set_profile() → set_yield_policy() / set_spin_last_us() の順。
	// set_profile() は spin_last_us と yield_policy を上書きするため。
	if (usleep_set_profile(profile) != 0)
	{
		fprintf(stderr, "usleep_set_profile(%d) failed\n", profile);
		return 1;
	}
	if (usleep_set_yield_policy(yield_policy) != 0)
	{
		fprintf(stderr, "usleep_set_yield_policy(%d) failed\n", yield_policy);
		return 1;
	}
	if (usleep_set_spin_last_us(spin_last_us) != 0)
	{
		fprintf(stderr, "usleep_set_spin_last_us(%u) failed\n", spin_last_us);
		return 1;
	}

	// システム全体のタイマ分解能を記録しておく。これはプロセス横断の共有状態で、
	// 他プロセス（ブラウザ・メディア再生など）が timeBeginPeriod を呼んでいると
	// 勝手に変わる。WaitableTimer がどれだけ早着するか＝末尾スピンの長さ＝CPU% が
	// これに直接効くので、記録しないと測定結果を後から再現・比較できない。
	unsigned res_min = 0, res_max = 0, res_cur = 0;
	int res_ok = usleep_query_nt_resolution(&res_min, &res_max, &res_cur);

	// 環境変数で指定されたときだけ明示的に分解能を握る。既定では触らない
	// （＝他プロセス任せの素の状態を測る）。要求どおり握れたかは後で検証する。
	// 握れていない条件で測った数値を「0.5ms で測定」と書くと嘘になるため。
	unsigned req_res	  = 0;		// 0 = 触っていない
	bool	 res_acquired = false;
	{
		// getenv は MSVC の /W4 /WX で C4996 になるため Win32 API で読む。
		char envbuf[32] = { 0 };
		const DWORD envlen =
			GetEnvironmentVariableA("USLEEP_BENCH_NT_RES_100NS", envbuf, (DWORD)sizeof(envbuf));
		const char* env = (envlen > 0 && envlen < sizeof(envbuf)) ? envbuf : nullptr;
		if (env && *env)
		{
			// 大文字小文字を無視した比較。_stricmp は -std=c++17（__STRICT_ANSI__）の
			// MinGW で宣言が隠れることがあるので自前で書く。
			if (eq_nocase(env, "max") || eq_nocase(env, "finest"))
				req_res = res_max;
			else
				req_res = (unsigned)strtoul(env, nullptr, 10);

			if (req_res == 0)
			{
				fprintf(stderr, "USLEEP_BENCH_NT_RES_100NS=%s is invalid\n", env);
				return 1;
			}
			if (usleep_init_nt_resolution(req_res) != 0)
			{
				fprintf(stderr, "usleep_init_nt_resolution(%u) failed\n", req_res);
				return 1;
			}
			res_acquired = true;
			// 設定後の実効値を測定条件として記録し直す
			res_ok = usleep_query_nt_resolution(&res_min, &res_max, &res_cur);
		}
	}

	const double cyc_per_us = uslp_cpu::calibrate_cycles_per_us();
	const bool	 have_cyc	= (cyc_per_us > 0.0);
	if (!have_cyc)
	{
		fprintf(stderr,
			"warning: QueryThreadCycleTime unavailable or calibration failed; "
			"cycle-based CPU%% columns will be 0\n");
	}

	// 設定・統計はともにスレッドローカル。設定・リセット・待機・読み出しを
	// すべてこのスレッドで完結させること（別スレッドから読むと 0 になる）。
	usleep_stats_t s_prev{}, s_now{}, s_start{};
	usleep_reset_stats();
	usleep_get_stats(&s_prev);
	s_start = s_prev;

	// ループ内でヒープを触らないよう、記録先は先に確保しておく。
	std::vector<Sample> rec;
	rec.reserve((size_t)iters);

	uslp_cpu::Span span;
	span.start();
	const uint64_t cyc0	 = span.cyc0;
	const uint64_t wall0 = span.wall0;

	uint64_t next = usleep_now_steady_us();

	// ---- 測定ループ（この中では出力しない） ----
	for (int i = 0; i < iters; ++i)
	{
		next += tick_us;
		usleep_until_steady_us(next);

		const uint64_t now	= usleep_now_steady_us();
		const uint64_t cyc	= thread_cycles();
		const uint64_t wall = qpc_now_us();

		// 統計差分（スレッドローカル）
		usleep_get_stats(&s_now);

		Sample s;
		s.late_us	= (now > next) ? (now - next) : 0ULL; // 早着は 0 丸め
		s.wall_d_us = wall - wall0;
		s.cyc_d		= cyc - cyc0;
		s.d_spin	= s_now.spin_relax			- s_prev.spin_relax;
		s.d_swth	= s_now.yield_switch		- s_prev.yield_switch;
		s.d_s0		= s_now.yield_sleep0		- s_prev.yield_sleep0;
		s.d_s1		= s_now.yield_sleep1		- s_prev.yield_sleep1;
		s.d_timer	= s_now.waitable_timer_uses - s_prev.waitable_timer_uses;
		rec.push_back(s);

		s_prev = s_now;
	}

	// ---- 総計（測定ループ直後に確定させる。以降の printf は含めない） ----
	span.stop();

	// 測定中に分解能が動いていないかを確認する（他プロセスが握り直しうる）。
	unsigned res_cur_end = 0;
	usleep_query_nt_resolution(nullptr, nullptr, &res_cur_end);

	const uint64_t wall_total = span.wall_us;
	const uint64_t thr_total  = span.thr_100ns;
	const uint64_t cyc_total  = span.cycles;

	const double cyc_us		  = have_cyc ? span.cycle_cpu_us(cyc_per_us) : 0.0;
	const double cyc_pct	  = have_cyc ? span.cycle_cpu_pct(cyc_per_us) : 0.0;
	const double thr_pct	  = span.thread_cpu_pct();
	const double proc_pct	  = span.process_cpu_pct();
	const double proc_sys_pct = proc_pct / (double)ncpu; // システム全体(全論理コア)=100% 換算

	// ---- CSV 出力 ----
	printf("# label=%s profile=%d yield_policy=%d spin_last_us=%u tick_us=%llu iters=%d ncpu=%u\n",
		label, profile, yield_policy, spin_last_us, (unsigned long long)tick_us, iters, ncpu);
	printf("# cum_thr_cpu_pct = cumulative measuring-thread CPU from QueryThreadCycleTime "
		   "(calibrated: %.1f cycles/us) / cumulative wall (QPC). 1 logical core = 100%%.\n",
		cyc_per_us);
	printf("# late_us = arrival - deadline (early arrival clamped to 0). "
		   "Other columns are per-iteration deltas of usleep_stats_t.\n");
	printf("iter,late_us,cum_thr_cpu_pct,spin_relax,yield_switch,yield_sleep0,yield_sleep1,timer_used\n");

	std::vector<uint64_t> lates;
	lates.reserve(rec.size());
	for (size_t i = 0; i < rec.size(); ++i)
	{
		const Sample& s = rec[i];
		const double cum_pct = (have_cyc && s.wall_d_us > 0)
			? ((double)s.cyc_d / cyc_per_us) / (double)s.wall_d_us * 100.0
			: 0.0;

		printf("%llu,%llu,%.2f,%llu,%llu,%llu,%llu,%llu\n",
			(unsigned long long)i,
			(unsigned long long)s.late_us,
			cum_pct,
			(unsigned long long)s.d_spin,
			(unsigned long long)s.d_swth,
			(unsigned long long)s.d_s0,
			(unsigned long long)s.d_s1,
			(unsigned long long)s.d_timer
		);
		lates.push_back(s.late_us);
	}

	// ---- サマリ ----
	uint64_t sum = 0;
	for (size_t i = 0; i < lates.size(); ++i) sum += lates[i];
	const double avg = lates.empty() ? 0.0 : (double)sum / (double)lates.size();

	std::vector<uint64_t> sorted = lates;
	std::sort(sorted.begin(), sorted.end());

	const uint64_t t_spin  = s_prev.spin_relax			- s_start.spin_relax;
	const uint64_t t_swth  = s_prev.yield_switch		- s_start.yield_switch;
	const uint64_t t_s0	   = s_prev.yield_sleep0		- s_start.yield_sleep0;
	const uint64_t t_s1	   = s_prev.yield_sleep1		- s_start.yield_sleep1;
	const uint64_t t_timer = s_prev.waitable_timer_uses - s_start.waitable_timer_uses;

	// 妥当性チェック用の派生量。
	//	 ns_per_spin_relax = スピン 1 周（YieldProcessor + QPC 読み）あたりの実測コスト。
	//	 QPC 読みが支配的なので数十 ns に収まるはず。これが桁で外れているときは
	//	 換算係数の実測か、CPU 時間の帰属のどちらかを疑うこと。
	const double cpu_us_per_iter  = rec.empty() ? 0.0 : cyc_us / (double)rec.size();
	const double ns_per_spin      = (t_spin > 0) ? (cyc_us * 1000.0) / (double)t_spin : 0.0;

	// サマリは CSV のコメント行（stdout）と stderr の両方に出す。
	// stderr 側はリダイレクトした CSV とは別に集計スクリプトから拾える。
	static const char* const kSummaryFmt =
		"#SUMMARY label=%s profile=%d yield=%d spin_us=%u tick_us=%llu iters=%d ncpu=%u\n"
		"#SUMMARY avg_late_us=%.2f p50_late_us=%llu p95_late_us=%llu p99_late_us=%llu max_late_us=%llu\n"
		"#SUMMARY thread_cycle_cpu_pct_of_1core=%.2f  (primary metric: QueryThreadCycleTime)\n"
		"#SUMMARY thread_cpu_pct_of_1core=%.2f  (reference only: GetThreadTimes, tick-sampled)\n"
		"#SUMMARY process_cpu_pct_of_1core=%.2f process_cpu_pct_of_system=%.2f  (all threads of the process)\n"
		"#SUMMARY cycles_per_us=%.1f thread_cycles=%llu thread_cycle_cpu_us=%.0f\n"
		"#SUMMARY wall_total_us=%llu thread_cpu_us=%llu cpu_us_per_iter=%.2f ns_per_spin_relax=%.1f\n"
		"#SUMMARY nt_resolution_100ns min=%u max=%u cur=%u cur_end=%u query_ok=%d requested=%u acquired=%d\n"
		"#SUMMARY spin_relax=%llu yield_switch=%llu yield_sleep0=%llu yield_sleep1=%llu timer_used=%llu\n";

	for (int pass = 0; pass < 2; ++pass)
	{
		FILE* fp = (pass == 0) ? stdout : stderr;
		fprintf(fp, kSummaryFmt,
			label, profile, yield_policy, spin_last_us, (unsigned long long)tick_us, iters, ncpu,
			avg,
			(unsigned long long)pct_of_sorted(sorted, 0.50),
			(unsigned long long)pct_of_sorted(sorted, 0.95),
			(unsigned long long)pct_of_sorted(sorted, 0.99),
			(unsigned long long)(sorted.empty() ? 0ULL : sorted.back()),
			cyc_pct,
			thr_pct,
			proc_pct, proc_sys_pct,
			cyc_per_us, (unsigned long long)cyc_total, cyc_us,
			(unsigned long long)wall_total,
			(unsigned long long)(thr_total / 10ULL),
			cpu_us_per_iter, ns_per_spin,
			res_min, res_max, res_cur, res_cur_end, res_ok, req_res, res_acquired ? 1 : 0,
			(unsigned long long)t_spin,
			(unsigned long long)t_swth,
			(unsigned long long)t_s0,
			(unsigned long long)t_s1,
			(unsigned long long)t_timer
		);
	}

	// 分解能はシステム全体の共有状態。握ったら必ず返す。
	if (res_acquired) usleep_shutdown_nt_resolution();
	return 0;
}
