# usleep_win — 実装仕様書 (Spec Sheet)

本ドキュメントは `usleep_win` ライブラリの内部設計・API 仕様・チューニングガイドをまとめた技術仕様書です。

---

## 目次

1. [内部設計](#内部設計)
2. [ハイブリッド待機方式](#ハイブリッド待機方式)
3. [API 仕様](#api-仕様)
4. [プロファイル定義と閾値](#プロファイル定義と閾値)
5. [チューニングガイド](#チューニングガイド)
6. [NT ネイティブ API によるタイマー分解能制御](#nt-ネイティブ-api-によるタイマー分解能制御)
7. [スレッドローカル設計](#スレッドローカル設計)
8. [DllMain クリーンアップ](#dllmain-クリーンアップ)

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
  ├─ usec >= timer_first_us → WaitableTimer + tail spin
  ├─ usec > prefer_spin_below (HR timer あり) → WaitableTimer + tail spin
  ├─ LOW_POWER → spin_with_yield_until_us(..., SLEEP1)
  └─ それ以外 → spin_with_yield_until_us(..., yield_policy)
```

### 時刻取得: `qpc_now_us()`

- `QueryPerformanceCounter` / `QueryPerformanceFrequency` を使用
- 周波数は初回呼び出し時に static 変数にキャッシュ（不変値）
- µs への変換は純粋整数演算 `(ticks / freq) * 1000000 + (ticks % freq) * 1000000 / freq`
- オーバーフロー保護: 商が `UINT64_MAX / 1000000` を超える場合は `UINT64_MAX` を返す

### タイマーハンドル: `get_timer_handle()`

- スレッドローカル `t_timer` にキャッシュ
- `CreateWaitableTimerExW` で `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` フラグを試行
- 成功すれば High-Resolution Timer（100ns 分解能）
- 失敗した場合は `CreateWaitableTimerW` でフォールバック
- `DllMain(DLL_THREAD_DETACH)` でハンドルを確実にクローズ

---

## ハイブリッド待機方式

### 概要

大きな待機時間は OS タイマーで粗く待ち、最後の数百 µs だけスピンで精密に詰める **二段構え** 方式。

### フロー詳細

1. **WaitableTimer フェーズ** (`usec >= timer_first_us`)
   - `SetWaitableTimer` で `(usec - spin_last_us)` 分だけ粗く待機
   - `WaitForSingleObject(INFINITE)` でブロック（CPU 消費ゼロ）

2. **テールスピンフェーズ** (`spin_last_us > 0`)
   - QPC で現在時刻を繰り返しポーリング
   - `YieldProcessor()` (x86: `PAUSE` / ARM64: `YIELD`) を毎イテレーション実行
   - 64 イテレーション毎に `yield_policy` に基づくリラックス処理を挿入

3. **フォールバック** (タイマーの取得/セットに失敗した場合)
   - `Sleep(ms)` + テールスピン
   - `usec < 1000` の場合はスピンオンリー

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
| `usleep_set_power_mode` | `int usleep_set_power_mode(int mode)` | スレッド電力モードを設定（`SetThreadInformation` 使用） |

### 統計 API

| 関数 | シグネチャ | 説明 |
|---|---|---|
| `usleep_get_stats` | `void usleep_get_stats(usleep_stats_t* out)` | スレッドローカル統計を取得 |
| `usleep_reset_stats` | `void usleep_reset_stats(void)` | スレッドローカル統計をリセット |

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
- `prefer_spin_below`: High-Resolution Timer が利用可能でも、この値未満はスピンオンリー
- `spin_last_us`: WaitableTimer 後のテールスピン長
- `yield_policy`: 64 イテレーション毎のリラックス方式

### `usleep_set_profile()` の副作用

| プロファイル | 設定される `spin_last_us` | 設定される `yield_policy` |
|---|---|---|
| `USLP_BALANCED` | 250 | `USLP_YIELD_SLEEP0` |
| `USLP_STRICT` | 400（既存値が 300 未満の場合） | `USLP_YIELD_SWITCH_THREAD` |
| `USLP_LOW_POWER` | 0 | `USLP_YIELD_SLEEP1` |

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

- `get_NtSetTimerResolution()` / `get_NtQueryTimerResolution()`: lazy-init static パターンで関数ポインタをキャッシュ
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

## スレッドローカル設計

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
| `g_has_hrtimer` | `std::atomic<bool>` | High-Resolution Timer の利用可否 |
| `g_nt_resolution_100ns` | `std::atomic<unsigned>` | NT API で設定した分解能 |

---

## DllMain クリーンアップ

### `DLL_THREAD_DETACH`
- スレッドローカルの `t_timer` ハンドルをクローズ

### `DLL_PROCESS_DETACH`
- `t_timer` ハンドルをクローズ
- `g_time_period_ms` の値で `timeEndPeriod()` を呼び出し
- `g_nt_resolution_100ns` の値で `NtSetTimerResolution(prev, FALSE, ...)` を呼び出し

これにより、アプリケーションが `usleep_shutdown_*` を明示的に呼ばなくても、DLL アンロード時にシステム全体の設定が安全に復元される。
