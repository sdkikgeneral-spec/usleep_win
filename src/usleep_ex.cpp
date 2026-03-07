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

static PFN_NtSetTimerResolution get_NtSetTimerResolution()
{
	static auto fn = (PFN_NtSetTimerResolution)GetProcAddress(
		GetModuleHandleW(L"ntdll.dll"), "NtSetTimerResolution");
	return fn;
}
static PFN_NtQueryTimerResolution get_NtQueryTimerResolution()
{
	static auto fn = (PFN_NtQueryTimerResolution)GetProcAddress(
		GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution");
	return fn;
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
#ifndef ThreadPowerThrottling
#define ThreadPowerThrottling (THREAD_INFORMATION_CLASS)11
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

struct UsleepConfig
{
	UsleepProfile	  profile	   = USLP_BALANCED;
	unsigned		  spin_last_us = 250;
	UsleepPowerMode   power_mode   = USLP_POWER_DEFAULT;
	UsleepYieldPolicy yield_policy = USLP_YIELD_SLEEP0;
};
static thread_local UsleepConfig t_cfg;

static inline LARGE_INTEGER qpc_freq()
{
	static LARGE_INTEGER f = []{ LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
	return f;
}
static inline uint64_t qpc_now_us()
{
	LARGE_INTEGER now; QueryPerformanceCounter(&now);
	const auto f = (uint64_t)qpc_freq().QuadPart;
	if (f == 0) return 0;
	const uint64_t ticks = (uint64_t)now.QuadPart;
	const uint64_t q = ticks / f;
	const uint64_t r = ticks % f;
	if (q > (UINT64_MAX / 1000000ULL)) return UINT64_MAX;
	const uint64_t base_us = q * 1000000ULL;
	const uint64_t rem_us = (uint64_t)(((long double)r * 1000000.0L) / (long double)f);
	if (base_us > (UINT64_MAX - rem_us)) return UINT64_MAX;
	return base_us + rem_us;
}
static inline LONGLONG us_to_100ns(uint64_t us) {
	const uint64_t k = 10ULL;
	if (us > (uint64_t)(INT64_MAX / k)) us = (uint64_t)(INT64_MAX / k);
	return -(LONGLONG)(us * k);
}

static HANDLE get_timer_handle()
{
	if (t_timer) return t_timer;
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	if (k32)
	{
		using PFN_CreateWaitableTimerExW = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
		auto p = reinterpret_cast<PFN_CreateWaitableTimerExW>(GetProcAddress(k32, "CreateWaitableTimerExW"));
		if (p)
		{
			HANDLE h = p(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (h)
			{
				g_has_hrtimer.store(true);
				return (t_timer = h);
			}
		}
	}
	HANDLE h = CreateWaitableTimerW(nullptr, FALSE, nullptr);
	if (h)
	{
		g_has_hrtimer.store(false);
		t_timer = h;
	}
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

	uint64_t timer_first_us;
	uint64_t prefer_spin_below;

	switch (prof)
	{
	case USLP_STRICT:	 timer_first_us = 1500; prefer_spin_below = 500; break;
	case USLP_LOW_POWER: timer_first_us = 1000; prefer_spin_below = 0;	 break;
	default:			 timer_first_us = 2000; prefer_spin_below = 200; break;
	}

	const uint64_t now_us = qpc_now_us();
	const uint64_t target_us = (usec > (UINT64_MAX - now_us)) ? UINT64_MAX : (now_us + usec);
	bool can_hr = g_has_hrtimer.load();

	if (usec >= timer_first_us || (can_hr && usec > prefer_spin_below))
	{
		HANDLE h = get_timer_handle();
		if (h)
		{
			uint64_t coarse_us = usec;
			if (spin_last_us > 0 && usec > spin_last_us) coarse_us = usec - spin_last_us;
			LARGE_INTEGER due; due.QuadPart = us_to_100ns(coarse_us);
			if (SetWaitableTimer(h, &due, 0, nullptr, nullptr, FALSE))
			{
				t_stat_timer_uses++;
				WaitForSingleObject(h, INFINITE);
				if (spin_last_us > 0)
				{
					spin_with_yield_until_us(target_us, 0, USLP_YIELD_NONE);
				}
				else if (prof != USLP_LOW_POWER)
				{
					while (qpc_now_us() < target_us) cpu_relax();
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
			if (spin_last_us > 0)
			{
				spin_with_yield_until_us(target_us, 0, USLP_YIELD_NONE);
			}
			return;
		}
	}

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

USLEEP_API void usleep_win(uint64_t usec)
{
	do_sleep_us(usec);
}

USLEEP_API void nsleep_win(uint64_t nsec)
{
	do_sleep_us(nsec / 1000ULL);
}

USLEEP_API uint64_t usleep_now_steady_us()
{
	return qpc_now_us();
}

USLEEP_API void usleep_until_steady_us(uint64_t target_us)
{
	uint64_t now = qpc_now_us();
	if (target_us <= now) return;
	do_sleep_us(target_us - now);
}

USLEEP_API int usleep_init_timer_resolution(unsigned int ms)
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

USLEEP_API void usleep_shutdown_timer_resolution()
{
	unsigned prev = g_time_period_ms.exchange(0);
	if (prev) timeEndPeriod(prev);
}

USLEEP_API int usleep_set_profile(int profile)
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

USLEEP_API int usleep_set_spin_last_us(unsigned int us)
{
	t_cfg.spin_last_us = us;
	return 0;
}

USLEEP_API int usleep_set_yield_policy(int policy)
{
	if (policy < USLP_YIELD_NONE || policy > USLP_YIELD_SLEEP1) return -1;
	t_cfg.yield_policy = (UsleepYieldPolicy)policy;
	return 0;
}

USLEEP_API int usleep_set_power_mode(int mode)
{
	if (mode < USLP_POWER_DEFAULT || mode > USLP_POWER_ECO) return -1;

	using PFN_SetThreadInformation = BOOL (WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	if (!k32)
	{
		t_cfg.power_mode = (UsleepPowerMode)mode;
		return 0;
	}
	auto pSetThreadInformation = (PFN_SetThreadInformation)GetProcAddress(k32, "SetThreadInformation");
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

USLEEP_API int usleep_query_nt_resolution(unsigned int* min_100ns, unsigned int* max_100ns, unsigned int* cur_100ns)
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

USLEEP_API int usleep_init_nt_resolution(unsigned int hundreds_ns)
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

USLEEP_API void usleep_shutdown_nt_resolution(void)
{
	unsigned prev = g_nt_resolution_100ns.exchange(0);
	if (!prev) return;
	auto fn = get_NtSetTimerResolution();
	if (fn)
	{
		ULONG cur = 0;
		fn((ULONG)prev, FALSE, &cur);
	}
}

USLEEP_API void usleep_get_stats(usleep_stats_t* out)
{
	if (!out) return;
	out->spin_relax 		 = t_stat_spin_relax;
	out->yield_switch		 = t_stat_yield_switch;
	out->yield_sleep0		 = t_stat_yield_sleep0;
	out->yield_sleep1		 = t_stat_yield_sleep1;
	out->waitable_timer_uses = t_stat_timer_uses;
}

USLEEP_API void usleep_reset_stats(void)
{
	t_stat_spin_relax	= 0;
	t_stat_yield_switch = 0;
	t_stat_yield_sleep0 = 0;
	t_stat_yield_sleep1 = 0;
	t_stat_timer_uses	= 0;
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
			unsigned prev = g_time_period_ms.exchange(0);
			if (prev) timeEndPeriod(prev);

			unsigned nt_prev = g_nt_resolution_100ns.exchange(0);
			if (nt_prev)
			{
				auto fn = get_NtSetTimerResolution();
				if (fn) { ULONG cur = 0; fn((ULONG)nt_prev, FALSE, &cur); }
			}
		}
	}
	return TRUE;
}
