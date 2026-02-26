#pragma once
#include <stdint.h>

//============================================================
// usleep_win.h
//	 WindowsでLinuxのusleep相当の挙動を“実用的な精度/負荷”で提供するDLL用ヘッダ
//	 - C ABI でエクスポート（C/C++どちらからも利用可）
//	 - 既定は BALANCED（サーバーのメインループ向け）
//	 - 微小待機は「Sleep(0)で同優先度に譲る」+「最後だけスピンで詰める」
//============================================================

#if defined(_WIN32) || defined(_WIN64)
  #ifdef USLEEPWIN_EXPORTS
	#define USLEEP_API __declspec(dllexport)
  #else
	#define USLEEP_API __declspec(dllimport)
  #endif
#else
  #define USLEEP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- プロファイル ----
enum UsleepProfile
{
	USLP_BALANCED = 0,
	USLP_STRICT   = 1,
	USLP_LOW_POWER= 2,
};

// ---- 電力モード ----
enum UsleepPowerMode
{
	USLP_POWER_DEFAULT = 0,
	USLP_POWER_PERF    = 1,
	USLP_POWER_ECO	   = 2,
};

// ---- イールド方針 ----
enum UsleepYieldPolicy
{
	USLP_YIELD_NONE 		 = 0,
	USLP_YIELD_SWITCH_THREAD = 1,
	USLP_YIELD_SLEEP0		 = 2,
	USLP_YIELD_SLEEP1		 = 3,
};

// ---- 統計構造体 ----
typedef struct usleep_stats_t
{
	uint64_t spin_relax;
	uint64_t yield_switch;
	uint64_t yield_sleep0;
	uint64_t yield_sleep1;
	uint64_t waitable_timer_uses;
} usleep_stats_t;

// ---- API ----
USLEEP_API void 	usleep_win(uint64_t usec);
USLEEP_API void 	nsleep_win(uint64_t nsec);
USLEEP_API uint64_t usleep_now_steady_us(void);
USLEEP_API void 	usleep_until_steady_us(uint64_t target_us);

USLEEP_API int	usleep_init_timer_resolution(unsigned int ms);
USLEEP_API void usleep_shutdown_timer_resolution(void);

USLEEP_API int	usleep_set_profile(int profile);
USLEEP_API int	usleep_set_spin_last_us(unsigned int us);
USLEEP_API int	usleep_set_yield_policy(int policy);
USLEEP_API int	usleep_set_power_mode(int mode);

USLEEP_API void usleep_get_stats(usleep_stats_t* out);
USLEEP_API void usleep_reset_stats(void);

#ifdef __cplusplus
}
#endif
