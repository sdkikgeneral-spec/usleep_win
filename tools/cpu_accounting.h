// SPDX-License-Identifier: MIT

//============================================================
// CPU 使用率の計測（ベンチとテストで共有するヘッダ）
//
//	 このヘッダはビルドシステムに登録されていない純粋なヘッダである。
//	 tools/bench_usleep_csv.cpp と tests/test_usleep.cpp の両方から include し、
//	 「ベンチが表示する CPU% と、テストが検証する CPU% が同じ計器である」
//	 ことをコンパイル時に保証する。別々に実装すると、テストが通っていても
//	 ベンチの数字は壊れている、という空振りになる。
//
//	 定義: 分子 = 測定スレッド自身の CPU 時間 / 分母 = 同区間の実時間(QPC)。
//		   「論理コア 1 個 = 100%」。単一スレッドの測定なので上限は 100%。
//
//	 このファイルは UTF-8 BOM 付きで保存すること（プロジェクト規約）。
//============================================================

#ifndef USLEEP_WIN_TOOLS_CPU_ACCOUNTING_H
#define USLEEP_WIN_TOOLS_CPU_ACCOUNTING_H

#include <windows.h>
#include <stdint.h>
#include <algorithm>

namespace uslp_cpu {

//============================================================
// なぜ GetThreadTimes だけでは足りないか
//	 GetThreadTimes / GetProcessTimes が返すのはスケジューラのクロックティック
//	 （既定 15.625ms、timeBeginPeriod 等で短縮されうる）ごとのサンプリング課金で、
//	 「そのティック割り込みの瞬間に走っていたスレッド」に 1 ティック分をまとめて
//	 課金する方式である。したがって
//	   - 1ms 周期で数百 µs だけ走るスレッドは、ティック割り込みと自分の実行区間が
//		 相関しない限り系統的に過小計上される（0% 近くに出うる）
//	   - 逆にティックと位相同期してしまうと過大計上される
//	 いずれも「短いバーストを高頻度で繰り返す」この種の負荷では誤差が桁で出る。
//
//	 QueryThreadCycleTime はコンテキストスイッチのたびにハードウェアのサイクル
//	 カウンタ差分を積算するので、サンプリング課金の問題が無い。ただし単位が
//	 「サイクル」なので、時間に直すには換算係数が要る（calibrate_cycles_per_us）。
//============================================================

// ---- QPC（周波数はブート中不変なので一度だけ取得し、ループ内では取らない） ----
inline uint64_t g_qpc_freq = 1;

inline void init_qpc()
{
	LARGE_INTEGER f;
	if (QueryPerformanceFrequency(&f) && f.QuadPart > 0)
		g_qpc_freq = (uint64_t)f.QuadPart;
}

inline uint64_t qpc_now_us()
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	// 商・剰余に分けた純整数演算（本体実装と同じ方針。乗算のオーバーフローを避ける）
	uint64_t t = (uint64_t)c.QuadPart;
	return (t / g_qpc_freq) * 1000000ULL + ((t % g_qpc_freq) * 1000000ULL) / g_qpc_freq;
}

// ---- 動的解決する Win32 API ----
//	 QueryThreadCycleTime は Vista+、GetActiveProcessorCount は Win7+。
//	 本 DLL の方針に合わせ、静的リンクせず GetProcAddress で解決して
//	 不在時はフォールバックする（古い Windows で起動不能にしないため）。
typedef BOOL  (WINAPI* PFN_QueryThreadCycleTime)(HANDLE, PULONG64);
typedef DWORD (WINAPI* PFN_GetActiveProcessorCount)(WORD);

inline PFN_QueryThreadCycleTime	   g_pQueryThreadCycleTime	  = nullptr;
inline PFN_GetActiveProcessorCount g_pGetActiveProcessorCount = nullptr;

inline void resolve_dynamic_apis()
{
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	if (!k32) return;
	// GetProcAddress の戻り値 FARPROC から関数ポインタへの変換は
	// -Wcast-function-type を避けるため void* を経由する（本体実装と同じ流儀）。
	g_pQueryThreadCycleTime =
		(PFN_QueryThreadCycleTime)(void*)GetProcAddress(k32, "QueryThreadCycleTime");
	g_pGetActiveProcessorCount =
		(PFN_GetActiveProcessorCount)(void*)GetProcAddress(k32, "GetActiveProcessorCount");
}

// 論理コア数。GetSystemInfo は現在のプロセッサグループしか見えず 64 コアで
// 頭打ちになるため、あれば GetActiveProcessorCount を使う。
inline unsigned logical_cpu_count()
{
	if (g_pGetActiveProcessorCount)
	{
		unsigned n = (unsigned)g_pGetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
		if (n) return n;
	}
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1u;
}

// FILETIME は 100ns 単位。読むたびに µs へ落とすと 1 回あたり最大 1µs の
// 切り捨て誤差が出るので、100ns のまま保持して最後にだけ換算する。
inline uint64_t ft_to_100ns(const FILETIME& ft)
{
	ULARGE_INTEGER u;
	u.LowPart  = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return u.QuadPart;
}

// 測定スレッド自身の CPU 時間[100ns]（サンプリング課金）
inline uint64_t thread_cpu_100ns()
{
	FILETIME c, e, k, u;
	if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
	return ft_to_100ns(k) + ft_to_100ns(u);
}

// プロセス全体（全スレッド合算）の CPU 時間[100ns]
inline uint64_t process_cpu_100ns()
{
	FILETIME c, e, k, u;
	if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
	return ft_to_100ns(k) + ft_to_100ns(u);
}

// 測定スレッドの実行サイクル数（コンテキストスイッチ時に積算される）
// API 不在時は 0 固定 → 呼び出し側でサイクル系の出力を無効化する。
inline uint64_t thread_cycles()
{
	if (!g_pQueryThreadCycleTime) return 0;
	ULONG64 c = 0;
	if (!g_pQueryThreadCycleTime(GetCurrentThread(), &c)) return 0;
	return (uint64_t)c;
}

//============================================================
// サイクル → µs の換算係数を実測する
//	 QueryThreadCycleTime の「サイクル」は公称周波数で割れる保証が無い
//	 （実装は不変 TSC またはそれに準じるカウンタ）。定数を仮定せず、
//	 ビジーループで cycles/µs を実測する。
//
//	 注意点:
//	  - 事前にウォームアップを回して周波数を上げてから測る。低クロック状態で
//		測ると係数が小さく出て、結果として CPU% が過大に出る。
//	  - 1 回だけだとプリエンプトされた回に外れ値が出るので 3 回測って中央値を取る。
//	  - 不変 TSC 前提の実装なので、係数は周波数変動に対して安定する。
//		逆に言えば 1 サイクル = 1 命令発行機会ではないため、この値を
//		CPU の実クロックとして解釈してはいけない。あくまで換算係数。
//============================================================
inline double calibrate_cycles_per_us(uint64_t window_us = 20000ULL)
{
	if (!g_pQueryThreadCycleTime) return 0.0;

	// ウォームアップ（クロックを上げる。値は捨てる）
	{
		const uint64_t w = qpc_now_us();
		while (qpc_now_us() - w < window_us) { /* ビジー */ }
	}

	double samples[3] = { 0.0, 0.0, 0.0 };
	for (int i = 0; i < 3; ++i)
	{
		const uint64_t c0 = thread_cycles();
		const uint64_t w0 = qpc_now_us();
		while (qpc_now_us() - w0 < window_us) { /* ビジー */ }
		const uint64_t w1 = qpc_now_us();
		const uint64_t c1 = thread_cycles();
		const uint64_t dw = w1 - w0;
		samples[i] = (dw > 0) ? (double)(c1 - c0) / (double)dw : 0.0;
	}
	std::sort(samples, samples + 3);
	return samples[1]; // 中央値
}

//============================================================
// 区間計測ヘルパー
//	 start() から stop() までの、測定スレッド自身の CPU 使用率を出す。
//	 必ず同一スレッドで start / stop すること（サイクル数もスレッド固有）。
//============================================================
struct Span
{
	uint64_t wall0 = 0, cyc0 = 0, thr0 = 0, proc0 = 0;
	uint64_t wall_us = 0, cycles = 0, thr_100ns = 0, proc_100ns = 0;

	void start()
	{
		thr0  = thread_cpu_100ns();
		proc0 = process_cpu_100ns();
		cyc0  = thread_cycles();
		wall0 = qpc_now_us();
	}

	void stop()
	{
		wall_us	   = qpc_now_us() - wall0;
		thr_100ns  = thread_cpu_100ns() - thr0;
		proc_100ns = process_cpu_100ns() - proc0;
		cycles	   = thread_cycles() - cyc0;
	}

	// 主値: QueryThreadCycleTime 由来。論理コア 1 個 = 100%
	double cycle_cpu_us(double cycles_per_us) const
	{
		return (cycles_per_us > 0.0) ? (double)cycles / cycles_per_us : 0.0;
	}
	double cycle_cpu_pct(double cycles_per_us) const
	{
		return (wall_us > 0) ? cycle_cpu_us(cycles_per_us) / (double)wall_us * 100.0 : 0.0;
	}
	// 参考値: GetThreadTimes 由来（ティックサンプリング課金）
	double thread_cpu_pct() const
	{
		return (wall_us > 0) ? ((double)thr_100ns / 10.0) / (double)wall_us * 100.0 : 0.0;
	}
	double process_cpu_pct() const
	{
		return (wall_us > 0) ? ((double)proc_100ns / 10.0) / (double)wall_us * 100.0 : 0.0;
	}
};

// QPC・動的 API の解決をまとめて行う（換算係数の実測は呼び出し側で明示的に）
inline void init()
{
	init_qpc();
	resolve_dynamic_apis();
}

} // namespace uslp_cpu

#endif // USLEEP_WIN_TOOLS_CPU_ACCOUNTING_H
