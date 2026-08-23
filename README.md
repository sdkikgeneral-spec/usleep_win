# Overview

This repository provides a lightweight, open-source Windows-compatible implementation of `usleep`,
maintained as a supporting utility within a broader education-oriented and cross-platform software ecosystem.

# usleep_win
高精度・低ジッタな Windows 用 `usleep()` 実装（WaitableTimer + QPC + YieldProcessor）

`usleep_win` は、Windows 上で **Linux の `usleep()` に相当する “実用的な精度と低負荷” の待機** を実現するための、小型・依存関係ゼロの C/C++ 向け DLL / ライブラリです。

English README: [document/README_en.md](./document/README_en.md)

- High-Resolution Waitable Timer（100ns）
- QueryPerformanceCounter (QPC)
- `YieldProcessor()`（x86: `PAUSE` / ARM64: `YIELD`）
- `Sleep(0)` / `SwitchToThread()` と短いスピンを組み合わせた **ハイブリッド方式**
- サーバー向けの低ジッタ **BALANCED** プロファイル（既定）
- ベンチ用の **統計カウンタ API**
- `usleep_until_steady_us()` による **締切スケジューリング**（ドリフト抑制）
- 呼び出し規約を固定した **C ABI**（`USLEEP_CALL` = MSVC では `__cdecl`）
- ヘッダと DLL の版を照合できる **バージョン照会 API**

> 注: Windows はハードリアルタイム OS ではありません。ミクロな待機は環境差（電源管理、仮想化、セキュリティソフト等）の影響を受けます。

---

## ✨ 特長

### 🔧 高精度・低ジッタ
- High-Resolution Waitable Timer が利用可能な環境では **100ns 分解能**
- 最後の **200–400µs** だけスピンで詰め、遅着を抑制しやすい

### 🧵 CPU に優しい待機
- スピン中に `YieldProcessor()` を発行
- 締切から遠い区間では **64 回に 1 回** `Sleep(0)` / `SwitchToThread()` を挟み、公平性と発熱のバランスを確保
- 締切直前（最後の `spin_last_us`）は譲らない純スピンで詰める
- ピュアなビジーウェイトを避け、サーバー負荷を抑制

### 🖥 プロファイル
| プロファイル | 説明 |
|---|---|
| **BALANCED**（既定） | サーバー/ゲーム/一般メインループ向け。低ジッタと公平性のバランス |
| **STRICT** | 低ジッタ重視。スピン厚め、`SwitchToThread()` 寄り |
| **LOW_POWER** | 省電力重視。`Sleep(1)` ベースでジッタは大きめ |

### ⚡ NT ネイティブ API による高精度タイマー分解能制御（オプション）
- `NtSetTimerResolution` (ntdll.dll) で **0.5 ms** 粒度のシステムタイマーを設定可能
- `timeBeginPeriod(1)` の 1 ms より細かく、より精密な待機が実現しやすくなる
- `usleep_query_nt_resolution()` で環境がサポートする最小分解能を事前確認できる

### 📊 ベンチ向け統計 API（スレッドローカル）
- `spin_relax`（PAUSE/YIELD の呼び出し回数）
- `yield_switch` / `yield_sleep0` / `yield_sleep1`
- `waitable_timer_uses`

### 🧩 状態のスコープ（重要）

設定と統計は **スレッドローカル**、タイマー分解能は **プロセス／システム全体** です。混同するとバグになります。

| 状態 | スコープ | 該当 API |
|---|---|---|
| プロファイル / スピン長 / イールド方針 / 電力モード | **スレッドローカル** | `usleep_set_profile` / `usleep_set_spin_last_us` / `usleep_set_yield_policy` / `usleep_set_power_mode` |
| 統計カウンタ | **スレッドローカル** | `usleep_get_stats` / `usleep_reset_stats` |
| WaitableTimer ハンドル | **スレッドローカル** | 内部で自動管理 |
| システムタイマー分解能 | **プロセス／システム全体** | `usleep_init_timer_resolution` / `usleep_init_nt_resolution` |

- 設定はワーカースレッドごとに行ってください。あるスレッドの設定は他スレッドに影響しません。
- **統計カウンタを別スレッドから読むと 0 が返ります。**
- タイマー分解能はシステム全体に影響します。`init_*` したら必ず対応する `shutdown_*` を呼んでください（OS は自動で戻しません）。

### 🔖 バージョン照会

ヘッダのマクロ（コンパイル時）と DLL の関数（実行時）を照合できます。

```cpp
// コンパイル時の値
// USLEEP_WIN_VERSION_MAJOR / _MINOR / _PATCH
// USLEEP_WIN_VERSION_STRING  → "0.2.1"
// USLEEP_WIN_VERSION_NUM     → (major<<16) | (minor<<8) | patch

if (usleep_win_version() != USLEEP_WIN_VERSION_NUM) {
    printf("DLL は %s、ヘッダは %s\n",
           usleep_win_version_string(), USLEEP_WIN_VERSION_STRING);
}
```

- `usleep_win_version()` は `uint32_t` のパック値（`major<<16 | minor<<8 | patch`）を返します
- `usleep_win_version_string()` が返すのは **静的な文字列リテラル**です。解放不要・スレッド安全
- 現在のバージョンは **0.2.1**（`meson.build` / `usleep_win.rc` / ヘッダで一致）

> **0.2.1 での修正**: `usleep_set_power_mode()` が内部定数（`ThreadPowerThrottling`）の
> 誤りにより **全モードで常に -1 を返し、電力モードが一度も適用されていなかった**問題を修正しました。

---

## 📦 ビルド & インストール

### Meson（推奨）
```bash
meson setup build --buildtype=release
meson compile -C build
meson test -C build
```
MSVC の場合は **x64 Native Tools コマンドプロンプト**から実行してください。

### MinGW (Makefile)
```bash
mingw32-make
mingw32-make test
```

### MSVC で sanity 実行が WinError 5 で拒否される環境
```powershell
powershell -ExecutionPolicy Bypass -File .\tools\meson_build_msvc.ps1 -RunTests
```

### リンク方法とマクロ

| 利用形態 | 利用者側で定義するマクロ |
|---|---|
| DLL をインポートライブラリ経由で使う | なし（既定で `__declspec(dllimport)`） |
| `src/usleep_ex.cpp` を自プロジェクトに取り込む／静的にリンクする | **`USLEEPWIN_STATIC`** |
| DLL 本体をビルドする | `USLEEPWIN_EXPORTS`（Meson / Makefile が自動で付与） |

`USLEEPWIN_STATIC` を定義すると `__declspec(dllimport)` が付かなくなります。
定義し忘れると、リンカが `__imp_` 付きシンボルを探して未解決になります。

### 呼び出し規約

公開 API はすべて `USLEEP_CALL`（MSVC では `__cdecl`）で固定してあります。
利用者が **`/Gz`(stdcall) や `/Gr`(fastcall) を既定にしてビルドしていても安全**です
（規約が食い違うと x86 でスタックが壊れます）。x64 / ARM64 は規約が 1 つしかないため実質無害な指定です。

> **注意**: `timeBeginPeriod(1)` はシステム全体に影響します。本ライブラリは既定で変更しません。必要時のみ `usleep_init_timer_resolution(1)` を明示的に呼び、終了時に `usleep_shutdown_timer_resolution()` で戻してください。

---

## 📝 使用例（Usage Examples）

### 1) **基本的なマイクロ秒スリープ**
```cpp
#include "usleep_win.h"
#include <iostream>

int main() {
    usleep_win(300);   // 300 µs 待機
    std::cout << "Slept 300 µs\n";
    return 0;
}
```

### 2) **1ms 周期のメインループ（締切方式・基本）**
```cpp
uint64_t next = usleep_now_steady_us();
const uint64_t tick = 1000; // 1ms

for (;;) {
    next += tick;
    usleep_until_steady_us(next);  // 締切まで待機（ドリフト抑制）
    do_main_logic();
}
```

### 3) **1ms 周期のメインループ（締切方式・高度版）**
```cpp
#include "usleep_win.h"
#include <iostream>

int main() {
    const uint64_t tick_us = 1000;  // 1 ms
    uint64_t next = usleep_now_steady_us();

    // 安定性重視の推奨設定
    usleep_set_profile(USLP_BALANCED);
    usleep_set_spin_last_us(250);          // 最後の 250 µs をスピン
    usleep_set_yield_policy(USLP_YIELD_SLEEP0);

    for (int i = 0; i < 5000; i++) {
        next += tick_us;
        usleep_until_steady_us(next);

        uint64_t now = usleep_now_steady_us();
        int64_t late = (now > next) ? (now - next) : 0;
        std::cout << "tick " << i << " late=" << late << "us\n";

        // オーバーランが大きい場合は再同期
        if (late > 5000) { // 5ms 超過
            next = now;
        }
    }
}
```

### 4) **プロファイル/イールド/スピンの調整**
```cpp
// 低ジッタ寄り
usleep_set_profile(USLP_STRICT);

// 省電力
usleep_set_profile(USLP_LOW_POWER);

// イールド方針の切り替え
usleep_set_yield_policy(USLP_YIELD_SLEEP0);        // 既定（同優先度に譲る）
usleep_set_yield_policy(USLP_YIELD_SWITCH_THREAD); // 同一CPUで局所的に譲る
usleep_set_yield_policy(USLP_YIELD_SLEEP1);        // 確実に譲る（ジッタは増加）

// 最後のスピン長（µs）
usleep_set_spin_last_us(250);  // 推奨: 200–400
```

> **⚠ 呼び出し順序に注意**: `usleep_set_profile()` は **`spin_last_us` と `yield_policy` を上書きします**。
> 個別に詰めたい場合は必ず **`set_profile()` → `set_spin_last_us()` / `set_yield_policy()`** の順で呼んでください。
> 逆順（`set_spin_last_us()` → `set_profile()`）では、指定した値がプロファイルの既定値で上書きされて消えます。

```cpp
// ❌ 消える
usleep_set_spin_last_us(400);
usleep_set_profile(USLP_BALANCED);   // spin_last_us が 250 に戻る

// ✅ 残る
usleep_set_profile(USLP_BALANCED);
usleep_set_spin_last_us(400);
```

### 5) **統計カウンタの読み取り（ベンチ用）**
```cpp
usleep_stats_t st{};
usleep_get_stats(&st);

printf("spin_relax=%llu\n", (unsigned long long)st.spin_relax);
printf("yield_sleep0=%llu\n", (unsigned long long)st.yield_sleep0);
```

> カウンタは呼び出したスレッドのものです。**別スレッドから読むと 0 が返ります。**

### 6) **High-Resolution Timer を有効化（任意）**
```cpp
usleep_init_timer_resolution(1);   // 必要時のみ（システム全体に影響）
// ...
usleep_shutdown_timer_resolution();
```

### 7) **NT ネイティブ API で 0.5 ms 粒度のタイマー分解能を設定（任意・高精度版）**

`timeBeginPeriod(1)` は最小 1 ms 粒度ですが、`usleep_init_nt_resolution` を使うと
多くの環境で **0.5 ms (5000 × 100 ns)** まで細かくなり、待機精度が向上します。

```cpp
// 1. まず環境がサポートする分解能を確認する
unsigned min_res = 0, max_res = 0, cur_res = 0;
if (usleep_query_nt_resolution(&min_res, &max_res, &cur_res) == 0) {
    // min_res: 最も粗い値 (例: 156250 = 15.625 ms)
    // max_res: 最も細かい値 (例: 5000  =  0.5  ms)  ← これを使う
    // cur_res: 現在の設定
}

// 2. 最高精度で設定（max_res を使うか、固定値 5000 を指定）
usleep_init_nt_resolution(max_res);   // 環境依存の最大精度
// または
usleep_init_nt_resolution(5000);      // 0.5 ms 固定（対応環境のみ有効）

// 3. 通常通り待機（内部のタイマー粒度が向上した状態で動作）
usleep_win(800);   // 800 µs 待機

// 4. 終了時に必ず解除（システム全体への影響を戻す）
usleep_shutdown_nt_resolution();
```

> **注意**: `timeBeginPeriod` と同様にシステム全体に影響します。
> アプリ終了時、または不要になった時点で必ず `usleep_shutdown_nt_resolution()` を呼んでください。
> `NtSetTimerResolution` は undocumented API ですが、Win2000 以降で安定して動作します。

| API | 粒度 |
| --- | --- |
| `timeBeginPeriod(1)` | 最小 **1.0 ms** |
| `usleep_init_nt_resolution(5000)` | 最小 **0.5 ms**（対応環境） |

---

## 🔧 プロファイルのチューニングガイド

プロファイル別の閾値テーブル・チューニング詳細・ベンチマーク結果は **[実装仕様書 (specsheet.md)](./document/specsheet.md)** を参照してください。

| プロファイル | ジッタ | CPU | 公平性 | 用途 |
|---|---|---|---|---|
| **BALANCED**（既定） | ★★★ | ★★☆ | ★★★ | 1ms メインループ、ゲーム/サーバー |
| **STRICT** | ★★★★ | ★★★★ | ★★☆ | レイテンシ重視、オーディオ |
| **LOW_POWER** | ★☆☆☆ | ★☆☆☆ | ★★★★ | バックグラウンド、省電力 |

## 📊 ベンチマーク（CSV 出力）
`tools/bench_usleep_csv.cpp` を使うと、イテレーションごとに遅着/CPU%/譲り回数などを CSV で出力できます。

```bash
bench_usleep_csv.exe 2000 1000 200 2 > result.csv
```

出力列:
```
iter,late_us,cpu_pct,spin_relax,yield_switch,yield_sleep0,yield_sleep1,timer_used
```

ベンチマーク実測結果は **[テスト結果 (test_result.md)](./document/test_result.md)** を参照してください。

---

## 📁 ディレクトリ構成
```
include/
 └ usleep_win.h
src/
 └ usleep_ex.cpp
resource/
 └ usleep_win.rc
tools/
 └ bench_usleep_csv.cpp
tests/
 └ test_usleep.cpp
```

---

## 📜 ライセンス
このリポジトリは MIT License で提供しています。詳細は [LICENSE](./LICENSE) を参照してください。

---

## 🤝 貢献
Issue / PR 歓迎です。最適化・改善案・計測結果の共有など、お待ちしています。

### ソースの文字コード
`src/` `include/` `tests/` `tools/` のソースは **UTF-8 BOM 付き**で保存してください。

- BOM を落とすと MSVC は実行環境の ANSI コードページ（日本語環境では CP932）としてソースを読みます
- とくに公開ヘッダ `include/usleep_win.h` は**利用者のビルド設定（`/utf-8` の有無）を選べない**ため、
  BOM が無いと利用者側で C4819 が出るうえ、日本語コメント直後の宣言が壊れる危険があります

### 計測結果を共有する場合
測定条件（CPU、OS ビルド、電源プラン、タイマー分解能の設定、同時実行している負荷）を必ず併記してください。
条件のない数値は比較できません。
