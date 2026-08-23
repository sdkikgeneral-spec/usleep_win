// SPDX-License-Identifier: MIT

#pragma once
#include <stdint.h>

//============================================================
// usleep_win.h
//	 WindowsでLinuxのusleep相当の挙動を“実用的な精度/負荷”で提供するDLL用ヘッダ
//	 - C ABI でエクスポート（C/C++どちらからも利用可）
//	 - 既定は BALANCED（サーバーのメインループ向け）
//	 - 微小待機は「Sleep(0)で同優先度に譲る」+「最後だけスピンで詰める」
//
//	 このファイルは UTF-8 BOM 付きで保存すること。
//	 公開ヘッダは利用者のビルド設定（/utf-8 の有無）を選べないため、BOM が無いと
//	 MSVC は実行環境の ANSI コードページ（日本語環境では CP932）として読み、
//	 C4819 を出したうえで日本語コメント直後の宣言を食い潰す危険がある。
//============================================================

//============================================================
// バージョン
//	 usleep_win_version() でロード中の DLL の値を取得し、
//	 USLEEP_WIN_VERSION_NUM（このヘッダのコンパイル時の値）と照合できる。
//============================================================
#define USLEEP_WIN_VERSION_MAJOR	0
#define USLEEP_WIN_VERSION_MINOR	2
#define USLEEP_WIN_VERSION_PATCH	1
#define USLEEP_WIN_VERSION_STRING	"0.2.1"

// 上位8bit=major / 中位8bit=minor / 下位8bit=patch にパックした比較用の値
#define USLEEP_WIN_VERSION_NUM \
	(((uint32_t)USLEEP_WIN_VERSION_MAJOR << 16) | \
	 ((uint32_t)USLEEP_WIN_VERSION_MINOR <<	 8) | \
	  (uint32_t)USLEEP_WIN_VERSION_PATCH)

//============================================================
// リンケージ / 呼び出し規約
//	 USLEEPWIN_EXPORTS : DLL 本体のビルド時に定義（dllexport）
//	 USLEEPWIN_STATIC  : 静的リンクで使う利用者が定義（dllimport を抑止）
//	 いずれも未定義      : DLL 利用者（dllimport）
//============================================================
#if defined(_WIN32) || defined(_WIN64)
  #if defined(USLEEPWIN_STATIC)
	#define USLEEP_API
  #elif defined(USLEEPWIN_EXPORTS)
	#define USLEEP_API __declspec(dllexport)
  #else
	#define USLEEP_API __declspec(dllimport)
  #endif
#else
  #define USLEEP_API
#endif

// 呼び出し規約は必ず明示する。利用者が /Gz(stdcall) や /Gr(fastcall) で
// ビルドしていると既定規約が変わり、x86 ではスタックが壊れるため。
// x64 / ARM64 は規約が 1 つしかないので実質無害な指定になる。
#if defined(_MSC_VER)
  #define USLEEP_CALL __cdecl
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(_M_IX86))
  // GCC/Clang の cdecl 属性は x86(32bit) 専用。x64/ARM64 に付けると
  // -Wattributes で「無視した」と警告されるため、32bit のときだけ付ける。
  #define USLEEP_CALL __attribute__((__cdecl__))
#else
  #define USLEEP_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

//============================================================
// 共通の規約
//	 - int を返す関数は 0 = 成功 / -1 = 失敗 で統一
//	 - 時間を表す引数の単位は名前の接尾辞（_us / _ms / _100ns）に従う
//	 - enum の値は ABI の一部。既存の値は変更せず、追加は必ず末尾に行うこと
//	 - 構造体 usleep_stats_t はサイズ・並びとも ABI の一部。
//		フィールドの追加・削除・並べ替えは既存バイナリを壊す
//============================================================

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

// ---- 待機 API（スレッドローカル設定に従う） ----
USLEEP_API void 	USLEEP_CALL usleep_win(uint64_t usec);
USLEEP_API void 	USLEEP_CALL nsleep_win(uint64_t nsec);
USLEEP_API uint64_t USLEEP_CALL usleep_now_steady_us(void);
USLEEP_API void 	USLEEP_CALL usleep_until_steady_us(uint64_t target_us);

// ---- タイマー分解能（プロセス/システム全体に影響） ----
// init したら必ず対応する shutdown を呼ぶこと。OS は自動で戻さない。
// スレッドローカルではないので、どのスレッドから呼んでも全スレッドに効く。
USLEEP_API int	USLEEP_CALL usleep_init_timer_resolution(unsigned int ms);
USLEEP_API void USLEEP_CALL usleep_shutdown_timer_resolution(void);

// NT ネイティブ API による高精度タイマー分解能制御 (100ns 単位)
// hundreds_ns の目安: 5000 = 500µs (最高精度), 10000 = 1ms, 0 = 解除
// usleep_query_nt_resolution で min/max/cur を事前確認推奨
// これらもプロセス/システム全体に影響する。
USLEEP_API int	USLEEP_CALL usleep_query_nt_resolution(unsigned int* min_100ns, unsigned int* max_100ns, unsigned int* cur_100ns);
USLEEP_API int	USLEEP_CALL usleep_init_nt_resolution(unsigned int hundreds_ns);
USLEEP_API void USLEEP_CALL usleep_shutdown_nt_resolution(void);

// ---- 設定 API（すべてスレッドローカル） ----
// 設定は呼び出したスレッドにのみ効く。ワーカースレッドごとに設定すること。
// 注意: usleep_set_profile() は spin_last_us と yield_policy を上書きする。
//		 個別に詰めたい場合は必ず set_profile() → set_spin_last_us() /
//		 set_yield_policy() の順で呼ぶこと。逆順では設定が消える。
USLEEP_API int	USLEEP_CALL usleep_set_profile(int profile);
USLEEP_API int	USLEEP_CALL usleep_set_spin_last_us(unsigned int us);
USLEEP_API int	USLEEP_CALL usleep_set_yield_policy(int policy);
USLEEP_API int	USLEEP_CALL usleep_set_power_mode(int mode);

// ---- 統計（スレッドローカル） ----
// カウンタは呼び出したスレッドのもの。別スレッドから読むと 0 が返る。
USLEEP_API void USLEEP_CALL usleep_get_stats(usleep_stats_t* out);
USLEEP_API void USLEEP_CALL usleep_reset_stats(void);

// ---- バージョン照会（スレッド非依存・いつでも呼べる） ----
// usleep_win_version() は USLEEP_WIN_VERSION_NUM と同じパック形式を返す。
// DLL を差し替える運用では、ヘッダ側の値と実行時の値を比較して不整合を検出できる。
USLEEP_API uint32_t 	USLEEP_CALL usleep_win_version(void);
USLEEP_API const char*	USLEEP_CALL usleep_win_version_string(void);

#ifdef __cplusplus
}
#endif
