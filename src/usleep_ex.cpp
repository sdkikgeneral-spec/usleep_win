// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <profileapi.h>
#include <processthreadsapi.h>
#include <atomic>
#include <stdint.h>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

#if defined(_MSC_VER)
  #include <intrin.h>
#endif

#ifndef USLEEPWIN_EXPORTS
#define USLEEPWIN_EXPORTS
#endif

#include "../include/usleep_win.h"

// ---- GetProcAddress の戻り値を関数ポインタ型へ変換する ----
// GetProcAddress は FARPROC（実体は引数なし関数ポインタ）を返すため、目的の
// シグネチャへ直接キャストすると GCC が -Wcast-function-type を出す。
// 汎用関数ポインタ型 void(*)(void) を経由するキャストは GCC が
// 「意図的な変換」として警告対象から除外すると明記しているため、それに倣う。
// 変換そのものの意味は従来のキャストと同一（Win32 の標準的なイディオム）。
template <typename Fn>
static inline Fn cast_proc(FARPROC p)
{
	return reinterpret_cast<Fn>(reinterpret_cast<void (*)(void)>(p));
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef CREATE_WAITABLE_TIMER_MANUAL_RESET
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#endif

// NT ネイティブ API (ntdll.dll) — undocumented だが Win2000 以降で安定して存在する
#ifndef NT_SUCCESS
typedef LONG NTSTATUS;
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#endif
typedef NTSTATUS (NTAPI* PFN_NtSetTimerResolution)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
typedef NTSTATUS (NTAPI* PFN_NtQueryTimerResolution)(PULONG MinimumResolution, PULONG MaximumResolution, PULONG CurrentResolution);

// ---- ntdll 関数ポインタの解決 ----
// 関数ローカル static（C++11 magic static）は初回初期化で CRT の once ロックを取るため、
// DllMain（ローダロック保持中）から到達しうる経路では絶対に使わない。
// ここではプレーンなプロセスグローバル atomic に保持し、DllMain 経路は
// 「解決済みなら使う／未解決なら何もしない」（peek_*）だけを行う。
static std::atomic<PFN_NtSetTimerResolution>   g_pNtSetTimerResolution{nullptr};
static std::atomic<PFN_NtQueryTimerResolution> g_pNtQueryTimerResolution{nullptr};
static std::atomic<bool> g_ntdll_resolved{false};

// 解決を試みる。DllMain からは呼ばないこと。
// 複数スレッドが同時に入っても同じ値を書くだけなので同期プリミティブは不要。
static void resolve_ntdll_procs()
{
	if (g_ntdll_resolved.load(std::memory_order_acquire)) return;
	HMODULE nt = GetModuleHandleW(L"ntdll.dll"); // ロードはしない（既にマップ済みのモジュールを引くだけ）
	if (nt)
	{
		g_pNtSetTimerResolution.store(
			cast_proc<PFN_NtSetTimerResolution>(GetProcAddress(nt, "NtSetTimerResolution")),
			std::memory_order_release);
		g_pNtQueryTimerResolution.store(
			cast_proc<PFN_NtQueryTimerResolution>(GetProcAddress(nt, "NtQueryTimerResolution")),
			std::memory_order_release);
	}
	g_ntdll_resolved.store(true, std::memory_order_release);
}

static PFN_NtSetTimerResolution get_NtSetTimerResolution()
{
	resolve_ntdll_procs();
	return g_pNtSetTimerResolution.load(std::memory_order_acquire);
}
static PFN_NtQueryTimerResolution get_NtQueryTimerResolution()
{
	resolve_ntdll_procs();
	return g_pNtQueryTimerResolution.load(std::memory_order_acquire);
}
// DllMain 経路専用: 解決済みのポインタを読むだけ。GetProcAddress も行わない。
static PFN_NtSetTimerResolution peek_NtSetTimerResolution()
{
	return g_pNtSetTimerResolution.load(std::memory_order_acquire);
}

#ifndef THREAD_POWER_THROTTLING_CURRENT_VERSION
#define THREAD_POWER_THROTTLING_CURRENT_VERSION 1
typedef struct _THREAD_POWER_THROTTLING_STATE
{
	ULONG Version;
	ULONG ControlMask;
	ULONG StateMask;
} THREAD_POWER_THROTTLING_STATE, *PTHREAD_POWER_THROTTLING_STATE;
#endif
#ifndef THREAD_POWER_THROTTLING_EXECUTION_SPEED
#define THREAD_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
// THREAD_INFORMATION_CLASS の ThreadPowerThrottling は列挙値 3
// （ThreadMemoryPriority=0, ThreadAbsoluteCpuPriority=1, ThreadDynamicCodePolicy=2）。
// 列挙子はマクロではないので #ifndef では検出できず、古い SDK 用のフォールバック定義が
// 常に有効になる。ここが 11 だと SetThreadInformation が ERROR_INVALID_PARAMETER(87) で
// 失敗し、usleep_set_power_mode() が常に -1 を返していた。
#ifndef ThreadPowerThrottling
#define ThreadPowerThrottling (THREAD_INFORMATION_CLASS)3
#endif

#ifndef YieldProcessor
  #if defined(_M_AMD64) || defined(_M_IX86)
	#include <immintrin.h>
	#define YieldProcessor() _mm_pause()
  #elif defined(_M_ARM64)
	#define YieldProcessor() __yield()
  #else
	#define YieldProcessor() ((void)0)
  #endif
#endif

static std::atomic<unsigned> g_time_period_ms{0};
static std::atomic<bool>	 g_has_hrtimer{false};
static std::atomic<unsigned> g_nt_resolution_100ns{0};

static thread_local uint64_t t_stat_spin_relax	 = 0;
static thread_local uint64_t t_stat_yield_switch= 0;
static thread_local uint64_t t_stat_yield_sleep0= 0;
static thread_local uint64_t t_stat_yield_sleep1= 0;
static thread_local uint64_t t_stat_timer_uses	= 0;

static inline void cpu_relax()
{
	YieldProcessor();
	t_stat_spin_relax++;
}

static thread_local HANDLE t_timer = NULL;

// ---- 待機バックエンドの強制（テスト専用の内部フック） ----
// 公開ヘッダ include/usleep_win.h には載せない。公開 ABI ではないので予告なく
// 変更・削除しうる（利用者向け API ではない）。
//
// 理由: HR WaitableTimer が使える通常の Windows 10 1803+ 環境では、
// do_sleep_us() の以下 2 経路が到達不能になり、テストで検証できない。
//   (a) LOW_POWER の純スピン分岐 … LOW_POWER は prefer_spin_below==0 なので
//       can_hr が真だと usec>0 の全てがタイマ経路へ吸われる
//   (b) SetWaitableTimer 失敗時の Sleep(ms) フォールバック … タイマ生成も
//       SetWaitableTimer も通常は成功する
// 「検証できないコードを黙って残す」のを避けるため、削除ではなく注入で
// 両経路をテストから踏めるようにする（HR タイマの無い古い Windows / WINE 等では
// 実際に到達しうる経路なので、削除は機能後退になる）。
//
// スコープはスレッドローカル。プロセス全体の g_has_hrtimer を書き換えると
// 他スレッドの挙動まで変わってしまい、状態スコープの規約に反する。
enum UsleepForceBackend
{
	USLP_FORCE_AUTO 	  = 0, // 実環境の判定に従う（既定）
	USLP_FORCE_NO_HRTIMER = 1, // HR タイマ非対応環境として振る舞う（経路選択のみ影響）
	USLP_FORCE_NO_TIMER   = 2, // WaitableTimer 自体が使えない環境として振る舞う
};
static thread_local int t_force_backend = USLP_FORCE_AUTO;

struct UsleepConfig
{
	UsleepProfile	  profile	   = USLP_BALANCED;
	unsigned		  spin_last_us = 250;
	UsleepPowerMode   power_mode   = USLP_POWER_DEFAULT;
	UsleepYieldPolicy yield_policy = USLP_YIELD_SLEEP0;
};
static thread_local UsleepConfig t_cfg;

// プロファイルごとの内部閾値テーブル
struct ProfileThresholds
{
	uint64_t timer_first_us;
	uint64_t prefer_spin_below;
};
static constexpr ProfileThresholds kProfileThresholds[] = {
	/* BALANCED  */ { 2000, 200 },
	/* STRICT    */ { 1500, 500 },
	/* LOW_POWER */ { 1000,   0 },
};

// QPC 周波数はブート中不変なので、競合して二重に取得しても同じ値になる。
// magic static を避けるためプレーンな atomic キャッシュにする。
static std::atomic<uint64_t> g_qpc_freq{0};
static inline uint64_t qpc_freq_hz()
{
	uint64_t f = g_qpc_freq.load(std::memory_order_relaxed);
	if (f == 0)
	{
		LARGE_INTEGER x;
		if (!QueryPerformanceFrequency(&x)) return 0;
		f = (uint64_t)x.QuadPart;
		g_qpc_freq.store(f, std::memory_order_relaxed);
	}
	return f;
}
static inline uint64_t qpc_now_us()
{
	LARGE_INTEGER now; QueryPerformanceCounter(&now);
	const uint64_t f = qpc_freq_hz();
	if (f == 0) return 0;
	const uint64_t ticks = (uint64_t)now.QuadPart;
	const uint64_t q = ticks / f;
	const uint64_t r = ticks % f;
	if (q > (UINT64_MAX / 1000000ULL)) return UINT64_MAX;
	const uint64_t base_us = q * 1000000ULL;
	const uint64_t rem_us  = (r * 1000000ULL) / f;
	if (base_us > (UINT64_MAX - rem_us)) return UINT64_MAX;
	return base_us + rem_us;
}
static inline LONGLONG us_to_100ns(uint64_t us) {
	const uint64_t k = 10ULL;
	if (us > (uint64_t)(INT64_MAX / k)) us = (uint64_t)(INT64_MAX / k);
	return -(LONGLONG)(us * k);
}

// ---- 高分解能 WaitableTimer の可用性判定（プロセス全体で一度だけ） ----
// 旧実装は get_timer_handle() が呼ばれるまで g_has_hrtimer が false のままで、
// 「まだ長い待機をしていないスレッド」では HR タイマ経路に入れなかった。
// ここでは可用性をプロセス全体で一度だけ確定させ、スレッドのウォームアップに依存させない。
using PFN_CreateWaitableTimerExW = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
static std::atomic<PFN_CreateWaitableTimerExW> g_pCreateWaitableTimerExW{nullptr};
static std::atomic<bool> g_hrtimer_probed{false};

// 競合した場合は複数スレッドが同じ判定を行うだけ（結果は同一）なのでロックは不要。
static bool probe_hrtimer_support()
{
	if (g_hrtimer_probed.load(std::memory_order_acquire))
		return g_has_hrtimer.load(std::memory_order_acquire);

	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	PFN_CreateWaitableTimerExW p = k32
		? cast_proc<PFN_CreateWaitableTimerExW>(GetProcAddress(k32, "CreateWaitableTimerExW"))
		: nullptr;

	bool ok = false;
	if (p)
	{
		HANDLE h = p(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (h)
		{
			ok = true;
			// 試作したハンドルは捨てずにこのスレッドのタイマとして使い回す
			if (!t_timer) t_timer = h;
			else CloseHandle(h);
		}
	}

	g_pCreateWaitableTimerExW.store(p, std::memory_order_release);
	g_has_hrtimer.store(ok, std::memory_order_release);
	g_hrtimer_probed.store(true, std::memory_order_release);
	return ok;
}

static HANDLE get_timer_handle()
{
	if (t_timer) return t_timer;
	if (probe_hrtimer_support())
	{
		// probe がこのスレッドのハンドルを確保している場合がある
		if (t_timer) return t_timer;
		auto p = g_pCreateWaitableTimerExW.load(std::memory_order_acquire);
		if (p)
		{
			HANDLE h = p(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (h) return (t_timer = h);
		}
	}
	// HR 不可、または HR ハンドル作成に失敗した場合は通常のタイマで代替する。
	// ここでプロセス全体の g_has_hrtimer を書き換えてはならない（スコープが違う）。
	HANDLE h = CreateWaitableTimerW(nullptr, FALSE, nullptr);
	if (h) t_timer = h;
	return t_timer;
}

static void spin_with_yield_until_us(
	uint64_t target_us,
	unsigned spin_last_us,
	UsleepYieldPolicy policy_for_coarse
)
{
	unsigned ctr = 0;
	for (;;)
	{
		uint64_t now = qpc_now_us();
		if (now >= target_us) break;
		uint64_t remain = target_us - now;
		if (remain > spin_last_us)
		{
			if ((++ctr & 63u) == 0u)
			{
				switch (policy_for_coarse)
				{
				case USLP_YIELD_SWITCH_THREAD: t_stat_yield_switch++; SwitchToThread(); break;
				case USLP_YIELD_SLEEP0: 	   t_stat_yield_sleep0++; Sleep(0); 		break;
				case USLP_YIELD_SLEEP1: 	   t_stat_yield_sleep1++; Sleep(1); 		break;
				case USLP_YIELD_NONE:
				default: cpu_relax(); break;
				}
			}
			else
			{
				cpu_relax();
				cpu_relax();
				cpu_relax();
			}
		}
		else
		{
			cpu_relax();
		}
	}
}

static void do_sleep_us(uint64_t usec)
{
	if (usec == 0)
	{
		t_stat_yield_switch++; SwitchToThread();
		return;
	}

	const UsleepProfile prof = t_cfg.profile;
	const unsigned spin_last_us = t_cfg.spin_last_us;
	const UsleepYieldPolicy yield_policy = t_cfg.yield_policy;

	const int pidx = (prof >= USLP_BALANCED && prof <= USLP_LOW_POWER) ? prof : USLP_BALANCED;
	const uint64_t timer_first_us   = kProfileThresholds[pidx].timer_first_us;
	const uint64_t prefer_spin_below = kProfileThresholds[pidx].prefer_spin_below;

	const uint64_t now_us = qpc_now_us();
	const uint64_t target_us = (usec > (UINT64_MAX - now_us)) ? UINT64_MAX : (now_us + usec);
	// HR タイマ可用性はプロセス単位で確定済み（未確定ならここで確定させる）。
	// テストフックで「HR なし」を強制されている場合はそちらを優先する。
	const int force = t_force_backend;
	const bool can_hr = (force == USLP_FORCE_AUTO) && probe_hrtimer_support();

	if (usec >= timer_first_us || (can_hr && usec > prefer_spin_below))
	{
		HANDLE h = (force == USLP_FORCE_NO_TIMER) ? NULL : get_timer_handle();
		if (h)
		{
			uint64_t coarse_us = usec;
			if (spin_last_us > 0 && usec > spin_last_us) coarse_us = usec - spin_last_us;
			LARGE_INTEGER due; due.QuadPart = us_to_100ns(coarse_us);
			if (SetWaitableTimer(h, &due, 0, nullptr, nullptr, FALSE))
			{
				t_stat_timer_uses++;
				WaitForSingleObject(h, INFINITE);
				if (prof != USLP_LOW_POWER)
				{
					spin_with_yield_until_us(target_us, 0, USLP_YIELD_NONE);
				}
				return;
			}
		}
		if (usec >= 1000)
		{
			uint64_t ms64 = usec / 1000ULL;
			if (ms64 == 0) ms64 = 1;
			if (ms64 > (uint64_t)MAXDWORD) ms64 = (uint64_t)MAXDWORD;
			DWORD ms = (DWORD)ms64;
			t_stat_yield_sleep1++;
			Sleep(ms);
			// Sleep(ms) は usec を ms へ切り捨てた値でしか眠らないため、
			// 最大 999µs 不足しうる。spin_last_us の値に関わらず必ず target まで
			// 詰める（spin_last_us==0 の LOW_POWER で早期リターンさせないため。
			// 以前はここが if (spin_last_us > 0) だったので LOW_POWER + タイマ不可の
			// 組み合わせで最大 999µs 早く返る契約違反があった）。
			// 詰める残りは定義上 1ms 未満に収まり、Sleep(ms) は実際にはほぼ必ず
			// オーバーシュートするためこのループは通常 0 回転で抜ける。
			spin_with_yield_until_us(target_us, 0, USLP_YIELD_NONE);
			return;
		}
		// usec < 1000 でタイマも使えない場合は下のスピン経路へフォールスルーする。
	}

	// ここから下は「タイマ経路に入らなかった」または「タイマが使えなかった」場合。
	// HR タイマのある環境では LOW_POWER(prefer_spin_below==0) がタイマ経路に
	// 吸われるため、この分岐は HR なし環境（またはテストフック）でのみ到達する。
	if (prof == USLP_LOW_POWER)
	{
		spin_with_yield_until_us(target_us, 0, USLP_YIELD_SLEEP1);
	}
	else
	{
		spin_with_yield_until_us(target_us, spin_last_us, yield_policy);
	}
}

extern "C" {

USLEEP_API void USLEEP_CALL usleep_win(uint64_t usec)
{
	do_sleep_us(usec);
}

USLEEP_API void USLEEP_CALL nsleep_win(uint64_t nsec)
{
	do_sleep_us(nsec / 1000ULL);
}

USLEEP_API uint64_t USLEEP_CALL usleep_now_steady_us()
{
	return qpc_now_us();
}

USLEEP_API void USLEEP_CALL usleep_until_steady_us(uint64_t target_us)
{
	uint64_t now = qpc_now_us();
	if (target_us <= now) return;
	do_sleep_us(target_us - now);
}

USLEEP_API int USLEEP_CALL usleep_init_timer_resolution(unsigned int ms)
{
	if (ms == 0)
	{
		unsigned prev = g_time_period_ms.exchange(0);
		if (prev) timeEndPeriod(prev);
		return 0;
	}
	MMRESULT r = timeBeginPeriod(ms);
	if (r == TIMERR_NOERROR)
	{
		unsigned prev = g_time_period_ms.exchange(ms);
		if (prev && prev != ms) timeEndPeriod(prev);
		return 0;
	}
	return -1;
}

static void shutdown_timer_resolution_impl()
{
	unsigned prev = g_time_period_ms.exchange(0);
	if (prev) timeEndPeriod(prev);
}

USLEEP_API void USLEEP_CALL usleep_shutdown_timer_resolution()
{
	shutdown_timer_resolution_impl();
}

USLEEP_API int USLEEP_CALL usleep_set_profile(int profile)
{
	if (profile < USLP_BALANCED || profile > USLP_LOW_POWER) return -1;
	t_cfg.profile = (UsleepProfile)profile;
	if (t_cfg.profile == USLP_LOW_POWER)
	{
		t_cfg.spin_last_us = 0;
		t_cfg.yield_policy = USLP_YIELD_SLEEP1;
	}
	else if (t_cfg.profile == USLP_STRICT)
	{
		if (t_cfg.spin_last_us < 300) t_cfg.spin_last_us = 400;
		t_cfg.yield_policy = USLP_YIELD_SWITCH_THREAD;
	}
	else
	{
		t_cfg.spin_last_us = 250;
		t_cfg.yield_policy = USLP_YIELD_SLEEP0;
	}
	return 0;
}

USLEEP_API int USLEEP_CALL usleep_set_spin_last_us(unsigned int us)
{
	// 上限 USLEEP_SPIN_LAST_US_MAX (10ms) の根拠:
	//  - spin_last_us は「末尾を YieldProcessor で埋める時間」であり、この区間は
	//    1 コアを 100% 占有する。上限が無いと set_spin_last_us(UINT_MAX) で
	//    あらゆる待機が純ビジースピンに化け、待機 API としての契約が壊れる。
	//  - Windows 既定のタイマ分解能は約 15.6ms。これを超える末尾スピンを許しても
	//    「タイマの粗さをスピンで隠す」という設計目的を超え、粗い待機部分が
	//    消えて全体がスピンになるだけで意味がない。
	//  - 一方 Windows の既定クォンタム（クライアントで約 20〜30ms）を超えて
	//    スピンし続けるとプリエンプトされ、かえって精度が落ちる。
	//  この 2 つの下側にあたる 10ms を上限とする。実用上の推奨値（250〜500µs）
	//  に対して十分な余裕があり、既定値・各プロファイルが設定する値
	//  （0 / 250 / 400）はすべて範囲内なので既存の正常系は挙動が変わらない。
	if (us > USLEEP_SPIN_LAST_US_MAX) return -1; // 範囲外では設定を変更しない
	t_cfg.spin_last_us = us;
	return 0;
}

USLEEP_API int USLEEP_CALL usleep_set_yield_policy(int policy)
{
	if (policy < USLP_YIELD_NONE || policy > USLP_YIELD_SLEEP1) return -1;
	t_cfg.yield_policy = (UsleepYieldPolicy)policy;
	return 0;
}

using PFN_SetThreadInformation = BOOL (WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
static std::atomic<PFN_SetThreadInformation> g_pSetThreadInformation{nullptr};
static std::atomic<bool> g_sti_resolved{false};
static PFN_SetThreadInformation get_SetThreadInformation()
{
	if (!g_sti_resolved.load(std::memory_order_acquire))
	{
		HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
		if (k32)
		{
			g_pSetThreadInformation.store(
				cast_proc<PFN_SetThreadInformation>(GetProcAddress(k32, "SetThreadInformation")),
				std::memory_order_release);
		}
		g_sti_resolved.store(true, std::memory_order_release);
	}
	return g_pSetThreadInformation.load(std::memory_order_acquire);
}

USLEEP_API int USLEEP_CALL usleep_set_power_mode(int mode)
{
	if (mode < USLP_POWER_DEFAULT || mode > USLP_POWER_ECO) return -1;

	auto pSetThreadInformation = get_SetThreadInformation();
	if (!pSetThreadInformation)
	{
		t_cfg.power_mode = (UsleepPowerMode)mode;
		return 0;
	}

	THREAD_POWER_THROTTLING_STATE state{};
	state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;

	if (mode == USLP_POWER_ECO)
	{
		state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
		state.StateMask   = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
	}
	else if (mode == USLP_POWER_PERF)
	{
		state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
		state.StateMask   = 0;
	}
	else
	{
		state.ControlMask = 0;
		state.StateMask   = 0;
	}

	BOOL ok = pSetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
	if (ok) t_cfg.power_mode = (UsleepPowerMode)mode;
	return ok ? 0 : -1;
}

USLEEP_API int USLEEP_CALL usleep_query_nt_resolution(unsigned int* min_100ns, unsigned int* max_100ns, unsigned int* cur_100ns)
{
	auto fn = get_NtQueryTimerResolution();
	if (!fn) return -1;
	ULONG mn = 0, mx = 0, cur = 0;
	NTSTATUS st = fn(&mn, &mx, &cur);
	if (!NT_SUCCESS(st)) return -1;
	if (min_100ns) *min_100ns = (unsigned)mn;
	if (max_100ns) *max_100ns = (unsigned)mx;
	if (cur_100ns) *cur_100ns = (unsigned)cur;
	return 0;
}

USLEEP_API int USLEEP_CALL usleep_init_nt_resolution(unsigned int hundreds_ns)
{
	auto fn = get_NtSetTimerResolution();
	if (!fn) return -1;

	if (hundreds_ns == 0)
	{
		// 0 は「解除」— 以前セットした値でリリースリクエストを送る
		unsigned prev = g_nt_resolution_100ns.exchange(0);
		if (prev)
		{
			ULONG cur = 0;
			fn((ULONG)prev, FALSE, &cur);
		}
		return 0;
	}

	ULONG cur = 0;
	NTSTATUS st = fn((ULONG)hundreds_ns, TRUE, &cur);
	if (!NT_SUCCESS(st)) return -1;

	unsigned prev = g_nt_resolution_100ns.exchange(hundreds_ns);
	if (prev && prev != hundreds_ns)
	{
		// 前のリクエストを解除（新しい値が既に有効）
		fn((ULONG)prev, FALSE, &cur);
	}
	return 0;
}

// allow_resolve=false のときは GetProcAddress を含む解決処理を一切行わない（DllMain 経路）。
// g_nt_resolution_100ns が非 0 ということは usleep_init_nt_resolution() が成功済み＝
// ポインタは既に解決されているので、DllMain 経路でも peek で必ず取得できる。
static void shutdown_nt_resolution_impl(bool allow_resolve)
{
	unsigned prev = g_nt_resolution_100ns.exchange(0);
	if (!prev) return;
	auto fn = allow_resolve ? get_NtSetTimerResolution() : peek_NtSetTimerResolution();
	if (fn)
	{
		ULONG cur = 0;
		fn((ULONG)prev, FALSE, &cur);
	}
}

USLEEP_API void USLEEP_CALL usleep_shutdown_nt_resolution(void)
{
	shutdown_nt_resolution_impl(true);
}

USLEEP_API void USLEEP_CALL usleep_get_stats(usleep_stats_t* out)
{
	if (!out) return;
	out->spin_relax 		 = t_stat_spin_relax;
	out->yield_switch		 = t_stat_yield_switch;
	out->yield_sleep0		 = t_stat_yield_sleep0;
	out->yield_sleep1		 = t_stat_yield_sleep1;
	out->waitable_timer_uses = t_stat_timer_uses;
}

USLEEP_API void USLEEP_CALL usleep_reset_stats(void)
{
	t_stat_spin_relax	= 0;
	t_stat_yield_switch = 0;
	t_stat_yield_sleep0 = 0;
	t_stat_yield_sleep1 = 0;
	t_stat_timer_uses	= 0;
}

// ---- バージョン照会 ----
// ヘッダのマクロをそのまま返す。DLL を差し替えた利用者が、
// コンパイル時に見ていたヘッダと実際にロードされた DLL の版を照合できる。
USLEEP_API uint32_t USLEEP_CALL usleep_win_version(void)
{
	return (uint32_t)USLEEP_WIN_VERSION_NUM;
}

USLEEP_API const char* USLEEP_CALL usleep_win_version_string(void)
{
	return USLEEP_WIN_VERSION_STRING;
}

// ---- テスト専用の内部フック（公開ヘッダ非掲載・ABI 互換の保証なし） ----
// 到達不能になりがちな待機経路をテストから強制的に踏むためだけに存在する。
// mode は UsleepForceBackend の値。設定はスレッドローカル。
// 戻り値は他の setter と同じく 0 = 成功 / -1 = 範囲外。
USLEEP_API int USLEEP_CALL usleep_internal_force_wait_backend(int mode)
{
	if (mode < USLP_FORCE_AUTO || mode > USLP_FORCE_NO_TIMER) return -1;
	t_force_backend = mode;
	return 0;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
	if (reason == DLL_THREAD_DETACH || reason == DLL_PROCESS_DETACH)
	{
		if (t_timer)
		{
			CloseHandle(t_timer);
			t_timer = NULL;
		}

		if (reason == DLL_PROCESS_DETACH)
		{
			// ここはローダロック保持中。magic static の初回初期化・ヒープ確保・
			// 他 DLL のロード・同期プリミティブでの待機を発生させてはならない。
			// timeEndPeriod / NtSetTimerResolution はシステム全体のタイマ分解能を
			// 戻すため、プロセス終了時でも OS 任せにできず呼ぶ必要がある。
			shutdown_timer_resolution_impl();
			shutdown_nt_resolution_impl(false);
		}
	}
	return TRUE;
}
