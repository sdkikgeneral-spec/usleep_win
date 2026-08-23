# usleep_win — 実装仕様書 (Spec Sheet)

本ドキュメントは `usleep_win` ライブラリの内部設計・API 仕様・チューニングガイドをまとめた技術仕様書です。

対象バージョン: **v0.2.1**（`meson.build` の `version:` / `resource/usleep_win.rc` の FILEVERSION / `USLEEP_WIN_VERSION_STRING` と一致）

---

## 目次

1. [内部設計](#内部設計)
2. [ハイブリッド待機方式](#ハイブリッド待機方式)
3. [API 仕様](#api-仕様)
4. [プロファイル定義と閾値](#プロファイル定義と閾値)
5. [チューニングガイド](#チューニングガイド)
6. [NT ネイティブ API によるタイマー分解能制御](#nt-ネイティブ-api-によるタイマー分解能制御)
7. [状態のスコープ（スレッドローカル / プロセス全体）](#状態のスコープスレッドローカル--プロセス全体)
8. [DllMain クリーンアップ](#dllmain-クリーンアップ)
9. [ビルドと利用形態](#ビルドと利用形態)

---

## 内部設計

### アーキテクチャ概要

```
呼び出し元
  │
  ▼
usleep_win(usec) / usleep_until_steady_us(target)
  │
  ▼
do_sleep_us(usec)
  ├─ usec == 0                                        → SwitchToThread()
  ├─ usec >= timer_first_us                           → WaitableTimer + tail spin
  ├─ can_hr && usec > prefer_spin_below               → WaitableTimer + tail spin
  ├─ LOW_POWER                                        → spin_with_yield_until_us(..., SLEEP1)
  └─ それ以外                                          → spin_with_yield_until_us(..., yield_policy)
```

分岐条件は 1 本の式で表現される。

```cpp
if (usec >= timer_first_us || (can_hr && usec > prefer_spin_below))
```

`can_hr` は `probe_hrtimer_support()` の戻り値（高分解能 WaitableTimer の可用性）。
閾値 `timer_first_us` / `prefer_spin_below` は `kProfileThresholds[]` から引く。

### 時刻取得: `qpc_now_us()`

- `QueryPerformanceCounter` / `QueryPerformanceFrequency` を使用
- 周波数はブート中不変なので、プロセスグローバルな `std::atomic<uint64_t> g_qpc_freq` にキャッシュ
  （関数ローカル `static` の magic static は CRT の once ロックを取るため意図的に避けている）
- µs への変換は純粋整数演算 `(ticks / freq) * 1000000 + (ticks % freq) * 1000000 / freq`
- オーバーフロー保護: 商が `UINT64_MAX / 1000000` を超える場合は `UINT64_MAX` を返す

### 高分解能タイマーの可用性判定: `probe_hrtimer_support()`

- `CreateWaitableTimerExW` を `kernel32.dll` から `GetProcAddress` で動的解決し、
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` 付きで実際に 1 本作成できるかを試す
- 判定は **プロセス全体で一度だけ**（`g_hrtimer_probed` / `g_has_hrtimer`）行い、以後不変
- 試作したハンドルは破棄せず、そのスレッドの `t_timer` として使い回す
- 判定は `do_sleep_us()` の入口で行われるため、**短い待機しかしないスレッドでも
  可用性が正しく反映される**（旧実装は `get_timer_handle()` が一度も呼ばれないスレッドで
  `g_has_hrtimer` が false のままとなり、HR タイマー経路に入れず純スピンし続けていた）
- 競合時は複数スレッドが同じ判定を行うだけなので同期プリミティブは使わない

### タイマーハンドル: `get_timer_handle()`

- スレッドローカル `t_timer` にキャッシュ
- `probe_hrtimer_support()` が真なら `CreateWaitableTimerExW` +
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` でハンドルを作成（100ns 分解能）
- 偽、または HR ハンドル作成に失敗した場合は `CreateWaitableTimerW` でフォールバック
  （このときプロセス全体の `g_has_hrtimer` は書き換えない。スコープが異なるため）
- `DllMain(DLL_THREAD_DETACH)` でハンドルを確実にクローズ

---

## ハイブリッド待機方式

### 概要

大きな待機時間は OS タイマーで粗く待ち、最後の数百 µs だけスピンで精密に詰める **二段構え** 方式。

### フロー詳細

1. **WaitableTimer フェーズ** (`usec >= timer_first_us || (can_hr && usec > prefer_spin_below)`)
   - `SetWaitableTimer` で `(usec - spin_last_us)` 分だけ粗く待機
     （`spin_last_us == 0` または `usec <= spin_last_us` のときは `usec` そのまま）
   - `WaitForSingleObject(INFINITE)` でブロック（CPU 消費ゼロ）

2. **テールスピンフェーズ**
   - タイマー復帰後は `spin_with_yield_until_us(target, 0, USLP_YIELD_NONE)` を呼ぶため、
     **締切までは譲らない純スピン**（`YieldProcessor()` のみ）
   - `LOW_POWER` プロファイルではこのテールスピンを行わず、タイマー復帰で即座に返る
   - タイマーを使わないスピン経路では、残り時間が `spin_last_us` を超えている間だけ
     64 イテレーションに 1 回 `yield_policy` に基づくリラックス処理を挿入し、
     残りが `spin_last_us` 以下になったら純スピンに切り替える

3. **フォールバック** (タイマーの取得/セットに失敗した場合)
   - `usec >= 1000` なら `Sleep(usec / 1000)` + 純スピンで締切まで詰める
   - `usec < 1000` の場合はスピン経路（4. と同じ）へフォールスルー

4. **スピン経路**（タイマー条件を満たさない場合）
   - `LOW_POWER` は `spin_with_yield_until_us(target, 0, USLP_YIELD_SLEEP1)`
   - それ以外は `spin_with_yield_until_us(target, spin_last_us, yield_policy)`

### スピンリラックスの動作

```
spin_with_yield_until_us():
  64回に1回:
    SLEEP0      → Sleep(0)          … 同優先度スレッドに譲る
    SWITCH_THREAD → SwitchToThread()  … 同CPUの他スレッドに局所的に譲る
    SLEEP1      → Sleep(1)          … 確実に譲る（~1ms ジッタ増加）
    NONE        → cpu_relax()       … 譲らない（純スピン）
  それ以外:
    cpu_relax() × 3  … YieldProcessor 3回
```

---

## API 仕様

### リンケージと呼び出し規約

公開 API はすべて `extern "C"` で、次の 2 つのマクロを介して宣言される。

```c
USLEEP_API <戻り値> USLEEP_CALL <関数名>(...);
```

| マクロ | 定義 | 用途 |
|---|---|---|
| `USLEEP_API` | `__declspec(dllexport)` / `__declspec(dllimport)` / 空 | リンケージ |
| `USLEEP_CALL` | MSVC: `__cdecl` / GCC・Clang(x86): `__attribute__((__cdecl__))` / その他: 空 | 呼び出し規約 |

- `USLEEPWIN_EXPORTS` を定義してビルドすると `dllexport`（DLL 本体側）
- `USLEEPWIN_STATIC` を定義すると `dllimport` を抑止（静的リンク／ソース直接取り込み時）
- どちらも未定義なら `dllimport`（DLL 利用者側の既定）
- 呼び出し規約を明示しているため、**利用者が `/Gz`(stdcall) や `/Gr`(fastcall) で
  ビルドしていても x86 でスタックが壊れない**。x64 / ARM64 は規約が 1 つしかないため実質無害な指定になる。

### バージョン照会

ヘッダ側のマクロ（コンパイル時の値）と DLL 側の関数（実行時の値）を照合できる。

| 名前 | 種別 | 値 / シグネチャ | 説明 |
|---|---|---|---|
| `USLEEP_WIN_VERSION_MAJOR` | マクロ | `0` | メジャー |
| `USLEEP_WIN_VERSION_MINOR` | マクロ | `2` | マイナー |
| `USLEEP_WIN_VERSION_PATCH` | マクロ | `1` | パッチ |
| `USLEEP_WIN_VERSION_STRING` | マクロ | `"0.2.1"` | 文字列表現 |
| `USLEEP_WIN_VERSION_NUM` | マクロ | `major<<16 \| minor<<8 \| patch` | 比較用のパック値 |
| `usleep_win_version` | 関数 | `uint32_t usleep_win_version(void)` | ロード中の DLL のパック値 |
| `usleep_win_version_string` | 関数 | `const char* usleep_win_version_string(void)` | ロード中の DLL のバージョン文字列 |

- `usleep_win_version_string()` が返すのは静的な文字列リテラル。**解放不要・スレッド安全**
- どちらの関数もスレッド非依存で、初期化前でも呼べる
- DLL を差し替える運用では `usleep_win_version() != USLEEP_WIN_VERSION_NUM` で不整合を検出できる

### コア関数

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `usleep_win` | `void usleep_win(uint64_t usec)` | 指定マイクロ秒だけ待機 |
| `nsleep_win` | `void nsleep_win(uint64_t nsec)` | 指定ナノ秒だけ待機（内部で µs に変換） |
| `usleep_now_steady_us` | `uint64_t usleep_now_steady_us(void)` | QPC ベースの現在時刻を µs で返す |
| `usleep_until_steady_us` | `void usleep_until_steady_us(uint64_t target_us)` | target_us まで待機（締切スケジューリング） |

### タイマー分解能制御

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `usleep_init_timer_resolution` | `int usleep_init_timer_resolution(unsigned int ms)` | `timeBeginPeriod(ms)` でシステムタイマー粒度を設定。0 で解除。成功時 0、失敗時 -1 |
| `usleep_shutdown_timer_resolution` | `void usleep_shutdown_timer_resolution(void)` | 設定済みのタイマー粒度を解除 |
| `usleep_query_nt_resolution` | `int usleep_query_nt_resolution(unsigned int* min_100ns, unsigned int* max_100ns, unsigned int* cur_100ns)` | NT API で最小/最大/現在のタイマー分解能（100ns 単位）を取得 |
| `usleep_init_nt_resolution` | `int usleep_init_nt_resolution(unsigned int hundreds_ns)` | NT API でタイマー分解能を設定（100ns 単位）。0 で解除 |
| `usleep_shutdown_nt_resolution` | `void usleep_shutdown_nt_resolution(void)` | NT API で設定した分解能を解除 |

### 設定関数

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `usleep_set_profile` | `int usleep_set_profile(int profile)` | プロファイルを設定（`USLP_BALANCED` / `USLP_STRICT` / `USLP_LOW_POWER`）。成功時 0 |
| `usleep_set_spin_last_us` | `int usleep_set_spin_last_us(unsigned int us)` | テールスピン長を µs で設定 |
| `usleep_set_yield_policy` | `int usleep_set_yield_policy(int policy)` | イールド方針を設定 |
| `usleep_set_power_mode` | `int usleep_set_power_mode(int mode)` | スレッド電力モードを設定（`SetThreadInformation` 使用）。成功時 0、失敗時 -1 |

> **呼び出し順序に注意**: `usleep_set_profile()` は `spin_last_us` と `yield_policy` を
> **上書きする**。個別に詰めたい場合は必ず `set_profile()` → `set_spin_last_us()` /
> `set_yield_policy()` の順で呼ぶこと。逆順では設定が消える。

> **v0.2.1 での修正**: 古い SDK 向けフォールバックで `THREAD_INFORMATION_CLASS` の
> `ThreadPowerThrottling` を誤って `11` と定義していたため、`SetThreadInformation` が
> `ERROR_INVALID_PARAMETER (87)` で失敗し、**`usleep_set_power_mode()` は全モードで常に -1 を返し、
> 電力モードは一度も適用されていなかった**。正しい列挙値 `3` に修正済み。
> なお `SetThreadInformation` 自体が解決できない古い Windows では、
> 従来どおり設定値をスレッドローカルに記録して 0 を返す（実際の絞りは行われない）。

### 統計 API

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `usleep_get_stats` | `void usleep_get_stats(usleep_stats_t* out)` | スレッドローカル統計を取得 |
| `usleep_reset_stats` | `void usleep_reset_stats(void)` | スレッドローカル統計をリセット |

> カウンタは呼び出したスレッドのもの。**別スレッドから読むと 0 が返る**。

### 統計構造体

```c
typedef struct usleep_stats_t {
    uint64_t spin_relax;          // YieldProcessor (PAUSE/YIELD) 呼び出し回数
    uint64_t yield_switch;        // SwitchToThread() 呼び出し回数
    uint64_t yield_sleep0;        // Sleep(0) 呼び出し回数
    uint64_t yield_sleep1;        // Sleep(1) 呼び出し回数
    uint64_t waitable_timer_uses; // WaitableTimer 使用回数
} usleep_stats_t;
```

### 列挙型

```c
enum UsleepProfile    { USLP_BALANCED=0, USLP_STRICT=1, USLP_LOW_POWER=2 };
enum UsleepPowerMode  { USLP_POWER_DEFAULT=0, USLP_POWER_PERF=1, USLP_POWER_ECO=2 };
enum UsleepYieldPolicy{ USLP_YIELD_NONE=0, USLP_YIELD_SWITCH_THREAD=1, USLP_YIELD_SLEEP0=2, USLP_YIELD_SLEEP1=3 };
```

---

## プロファイル定義と閾値

### 内部閾値テーブル

| プロファイル | `timer_first_us` | `prefer_spin_below` | 既定 `spin_last_us` | 既定 `yield_policy` |
|---|---:|---:|---:|---|
| **BALANCED** | 2000 | 200 | 250 | `USLP_YIELD_SLEEP0` |
| **STRICT** | 1500 | 500 | 400 | `USLP_YIELD_SWITCH_THREAD` |
| **LOW_POWER** | 1000 | 0 | 0 | `USLP_YIELD_SLEEP1` |

- `timer_first_us`: この値以上の待機で WaitableTimer を使用
- `prefer_spin_below`: High-Resolution Timer が利用可能な場合に、この値を超えたらタイマー経路に入る
  （`LOW_POWER` は 0 なので、HR タイマーが使える環境では 1µs 以上の待機がすべてタイマー経路になる）
- `spin_last_us`: WaitableTimer 後のテールスピン長
- `yield_policy`: 64 イテレーション毎のリラックス方式

### `usleep_set_profile()` の副作用

| プロファイル | 設定される `spin_last_us` | 設定される `yield_policy` |
|---|---|---|
| `USLP_BALANCED` | 250 | `USLP_YIELD_SLEEP0` |
| `USLP_STRICT` | 400（既存値が 300 未満の場合） | `USLP_YIELD_SWITCH_THREAD` |
| `USLP_LOW_POWER` | 0 | `USLP_YIELD_SLEEP1` |

`UsleepConfig` の初期値（`set_profile()` を一度も呼ばないスレッド）は
`profile = USLP_BALANCED` / `spin_last_us = 250` / `yield_policy = USLP_YIELD_SLEEP0` /
`power_mode = USLP_POWER_DEFAULT`。

---

## チューニングガイド

### 1. BALANCED（既定）
- ジッタ: ★★★ / CPU: ★★☆ / 公平性: ★★★
- 運用: 残りが長い区間は `Sleep(0)` で譲り、最後の ~250µs をスピン
- 用途: 1ms 周期のメインループ、I/O ポーリング、ゲーム/サーバーに最適

### 2. STRICT（低ジッタ）
- ジッタ: ★★★★ / CPU: ★★★★ / 公平性: ★★☆
- 運用: スピン厚め（300–500µs）、`SwitchToThread()` を周期的に使用
- 用途: レイテンシ重視、制御ループ、計測・オーディオ等

### 3. LOW_POWER（省電力）
- ジッタ: ★☆☆☆ / CPU: ★☆☆☆ / 公平性: ★★★★
- 運用: `Sleep(1)` 中心、スピンはゼロか短時間
- 用途: バックグラウンド処理、省電力サーバー

### スピン/イールド微調整のコツ
- `usleep_set_spin_last_us(200〜400)` : 長く→ジッタ↓/CPU↑、短く→CPU↓/ジッタ↑
- `usleep_set_yield_policy(...)` : `SLEEP0`（既定）、`SWITCH_THREAD`（局所性）、`SLEEP1`（省電力）

---

## NT ネイティブ API によるタイマー分解能制御

### 概要

`NtSetTimerResolution` / `NtQueryTimerResolution` (ntdll.dll) を動的リンクで使用。
`timeBeginPeriod(1)` の最小 1ms より細かく、**0.5ms (5000 × 100ns)** までシステムタイマーを設定可能。

### 内部実装

- `get_NtSetTimerResolution()` / `get_NtQueryTimerResolution()`: `GetModuleHandleW(L"ntdll.dll")`
  （ロードはしない）+ `GetProcAddress` で解決し、プロセスグローバルな `std::atomic` にキャッシュ。
  関数ローカル `static` の初回初期化は CRT の once ロックを取り、DllMain 経路でデッドロックしうるため使わない
- `peek_NtSetTimerResolution()`: DllMain 経路専用。解決済みのポインタを読むだけで `GetProcAddress` を行わない
- `g_nt_resolution_100ns`: `std::atomic<unsigned>` で現在設定値を管理
- 新しい値をセットする際、以前の値があればリリースリクエストを送信

### 注意事項

- **Undocumented API** だが Win2000 以降で安定して動作
- **システム全体に影響** — アプリ終了時に必ず `usleep_shutdown_nt_resolution()` で戻す
- `DllMain(DLL_PROCESS_DETACH)` でも安全にクリーンアップ

| API | 粒度 |
| --- | --- |
| `timeBeginPeriod(1)` | 最小 **1.0 ms** |
| `usleep_init_nt_resolution(5000)` | 最小 **0.5 ms**（対応環境） |

---

## 状態のスコープ（スレッドローカル / プロセス全体）

状態は 2 種類あり、混同するとバグになる。

| 状態 | スコープ | 該当 API |
|---|---|---|
| プロファイル / スピン長 / イールド方針 / 電力モード | **スレッドローカル** | `usleep_set_profile` / `usleep_set_spin_last_us` / `usleep_set_yield_policy` / `usleep_set_power_mode` |
| 統計カウンタ | **スレッドローカル** | `usleep_get_stats` / `usleep_reset_stats` |
| WaitableTimer ハンドル | **スレッドローカル** | 内部 (`get_timer_handle`) |
| システムタイマー分解能 | **プロセス／システム全体** | `usleep_init_timer_resolution` / `usleep_init_nt_resolution` とその `shutdown` |
| HR タイマー可用性 | **プロセス全体**（一度確定したら不変） | 内部 (`probe_hrtimer_support`) |

### スレッドローカル変数一覧

| 変数 | 型 | 用途 |
|---|---|---|
| `t_timer` | `HANDLE` | WaitableTimer ハンドル（スレッド毎に作成） |
| `t_cfg` | `UsleepConfig` | プロファイル・スピン長・イールド方針・電力モード |
| `t_stat_spin_relax` | `uint64_t` | PAUSE/YIELD カウンタ |
| `t_stat_yield_switch` | `uint64_t` | SwitchToThread カウンタ |
| `t_stat_yield_sleep0` | `uint64_t` | Sleep(0) カウンタ |
| `t_stat_yield_sleep1` | `uint64_t` | Sleep(1) カウンタ |
| `t_stat_timer_uses` | `uint64_t` | WaitableTimer 使用カウンタ |

### 設計意図

- **スレッドセーフ**: 各スレッドが独立した設定と統計を持つ（ロック不要）
- **DLL 利用時**: 呼び出しスレッド毎に WaitableTimer ハンドルが作成・キャッシュされる
- **プロファイル変更**: スレッドローカルなので他スレッドに影響しない

### グローバル変数

| 変数 | 型 | 用途 |
|---|---|---|
| `g_time_period_ms` | `std::atomic<unsigned>` | `timeBeginPeriod` で設定した値 |
| `g_has_hrtimer` | `std::atomic<bool>` | High-Resolution Timer の利用可否（`probe_hrtimer_support` が確定） |
| `g_hrtimer_probed` | `std::atomic<bool>` | 可用性判定を実施済みか |
| `g_pCreateWaitableTimerExW` | `std::atomic<関数ポインタ>` | 動的解決した `CreateWaitableTimerExW` |
| `g_nt_resolution_100ns` | `std::atomic<unsigned>` | NT API で設定した分解能 |
| `g_qpc_freq` | `std::atomic<uint64_t>` | QPC 周波数のキャッシュ |
| `g_pNtSetTimerResolution` / `g_pNtQueryTimerResolution` / `g_ntdll_resolved` | `std::atomic` | ntdll 関数ポインタと解決済みフラグ |
| `g_pSetThreadInformation` / `g_sti_resolved` | `std::atomic` | `SetThreadInformation` の関数ポインタと解決済みフラグ |

いずれも関数ローカル `static`（C++11 magic static）を使わないプレーンな `std::atomic` として保持している。
DllMain（ローダロック保持中）から到達しうる経路で CRT の once ロックを取らないための措置。

---

## DllMain クリーンアップ

### `DLL_THREAD_DETACH`
- スレッドローカルの `t_timer` ハンドルをクローズ

### `DLL_PROCESS_DETACH`
- `t_timer` ハンドルをクローズ
- `g_time_period_ms` の値で `timeEndPeriod()` を呼び出し
- `g_nt_resolution_100ns` の値で `NtSetTimerResolution(prev, FALSE, ...)` を呼び出し

これにより、アプリケーションが `usleep_shutdown_*` を明示的に呼ばなくても、DLL アンロード時にシステム全体の設定が安全に復元される。

> ただし `DLL_PROCESS_DETACH` は保険であり、通常は明示的に `usleep_shutdown_timer_resolution()` /
> `usleep_shutdown_nt_resolution()` を呼ぶこと。`init` したら必ず対応する `shutdown` を呼ぶのが原則。

---

## ビルドと利用形態

### ビルド方法

| 方法 | コマンド | 備考 |
|---|---|---|
| Meson（推奨） | `meson setup build --buildtype=release && meson compile -C build && meson test -C build` | MSVC は x64 Native Tools プロンプトから |
| MinGW / MSYS2 | `mingw32-make` / `mingw32-make test` | |
| MSVC ラッパー | `powershell -ExecutionPolicy Bypass -File .\tools\meson_build_msvc.ps1 -RunTests` | WinError 5 で sanity 実行が拒否される環境向け |

DLL 本体のビルドでは `USLEEPWIN_EXPORTS` を定義する（Meson / Makefile とも `-DUSLEEPWIN_EXPORTS` を付与済み）。

### 利用形態

| 形態 | 利用者側で定義するマクロ |
|---|---|
| DLL をインポートライブラリ経由で使う | なし（既定で `dllimport`） |
| `src/usleep_ex.cpp` を自プロジェクトに取り込む／静的にリンクする | `USLEEPWIN_STATIC` |

`USLEEPWIN_STATIC` を定義すると `USLEEP_API` が空になり、`__declspec(dllimport)` が付かなくなる。
定義し忘れると、リンカが `__imp_` 付きシンボルを探して未解決になる。

### ソースの文字コード

`src/` `include/` `tests/` `tools/` のソースは **UTF-8 BOM 付き** で保存すること。

- MSVC は BOM が無いと実行環境の ANSI コードページ（日本語環境では CP932）としてソースを読む
- DLL 本体は `meson.build` / `Makefile` 側で `/utf-8` を渡しているが、**公開ヘッダ
  `include/usleep_win.h` は利用者のビルド設定を選べない**。BOM が無いと利用者側で C4819 が出るうえ、
  日本語コメント直後の宣言が食い潰される危険がある
