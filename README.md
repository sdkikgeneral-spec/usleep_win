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

> **`yield_policy` が効くのはスピン経路に入った待機だけです。** WaitableTimer 経路に入ると、
> タイマー復帰後の末尾スピンは `USLP_YIELD_NONE` 固定で走り、`yield_policy` を参照しません。
> 条件の一覧は「4) プロファイル/イールド/スピンの調整」を参照してください。

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
// USLEEP_WIN_VERSION_STRING  → "0.2.2"
// USLEEP_WIN_VERSION_NUM     → (major<<16) | (minor<<8) | patch

if (usleep_win_version() != USLEEP_WIN_VERSION_NUM) {
    printf("DLL は %s、ヘッダは %s\n",
           usleep_win_version_string(), USLEEP_WIN_VERSION_STRING);
}
```

- `usleep_win_version()` は `uint32_t` のパック値（`major<<16 | minor<<8 | patch`）を返します
- `usleep_win_version_string()` が返すのは **静的な文字列リテラル**です。解放不要・スレッド安全
- 現在のバージョンは **0.2.2**（`meson.build` / `usleep_win.rc` / ヘッダで一致）

> **0.2.2 での変更**:
> - `usleep_set_spin_last_us()` に上限 `USLEEP_SPIN_LAST_US_MAX`（**10000µs = 10ms**）を追加しました。
>   上限を超える値では **-1 を返し、設定を変更しません**（従来は常に 0 を返していました）。
>   既定値 250 と各プロファイルが設定する 0 / 250 / 400 はすべて範囲内なので、正常系の挙動は変わりません。
> - High-Resolution WaitableTimer が使えない環境（Windows 10 1803 未満・WINE など）で
>   `Sleep(ms)` フォールバックに落ちたとき、`LOW_POWER`（`spin_last_us == 0`）では
>   µs→ms の切り捨て分（最大 999µs）を詰めずに返していた不具合を修正しました。
>   プロファイルに関わらず締切まで詰めます。
> - 静的ライブラリを正式なビルド成果物として追加し、`meson install` / `mingw32-make install` で
>   DLL・インポートライブラリ・静的ライブラリ・ヘッダ・pkg-config ファイルを配置できるようにしました。
> - GitHub Actions による CI（MSVC / MinGW × Meson / Makefile、警告はエラー扱い）を追加しました。

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

Meson / Makefile とも、共有ライブラリ・静的ライブラリ・テスト（共有版 / 静的版）・
ベンチをまとめてビルドします。テストは `meson test -C build` / `mingw32-make test` で
**共有版と静的版の両方**が実行されます。

### インストール（配布物の生成）

```bash
# Meson
meson install -C build --destdir stage

# Makefile（MinGW）
mingw32-make install PREFIX=/mingw64
```

インストールされるファイル:

| パス | 内容 |
|---|---|
| `bin/usleep_win.dll` | 共有ライブラリ（MinGW でも `name_prefix: ''` により `libusleep_win.dll` ではなく `usleep_win.dll`） |
| `lib/usleep_win.lib` | インポートライブラリ（MSVC）。MinGW + Meson では `usleep_win.dll.a`、MinGW + Makefile では `libusleep_win.a` |
| `lib/libusleep_win_static.a` | 静的ライブラリ（Meson は MSVC でもこの名前で出力します） |
| `include/usleep_win.h` | 公開ヘッダ |
| `lib/pkgconfig/usleep_win.pc` | pkg-config（共有版） |
| `lib/pkgconfig/usleep_win-static.pc` | pkg-config（静的版。Cflags に `-DUSLEEPWIN_STATIC` を含む） |

### CI

`.github/workflows/ci.yml` で次の 5 ジョブを **警告をエラー扱い**（Meson は `-Dwerror=true`、
Makefile は `-Werror`）で実行しています。

| ジョブ | 内容 |
|---|---|
| MSVC / Meson（`native=false` / `true`） | ビルド → テスト → `meson install` |
| MinGW / Meson（`native=false` / `true`） | 同上 + `pkgconf` による `.pc` の検証 |
| MinGW / Makefile | ビルド → テスト（共有 + 静的） → `install` → Meson との成果物名の突き合わせ |

CI のジョブは環境変数 **`USLEEP_TEST_CI=1`** を付けてテストを実行します。共有・仮想化された
ホストではスケジューリング遅延が跳ねるためで、このモードが緩めるのは**上限側のアサートだけ**です。

- 下限側（要求待機時間を満たしているか＝早期 return の検出）は緩めません
- 統計カウンタによる経路の証明（等値・下限）も緩めません
- 環境変数を付けなければ本番の精度要件のまま走ります

### リンク方法とマクロ

| 利用形態 | 利用者側で定義するマクロ |
|---|---|
| DLL をインポートライブラリ経由で使う | なし（既定で `__declspec(dllimport)`） |
| 静的ライブラリ（`libusleep_win_static.a`）にリンクする／`src/usleep_ex.cpp` を自プロジェクトに取り込む | **`USLEEPWIN_STATIC`** |
| DLL 本体をビルドする | `USLEEPWIN_EXPORTS`（Meson / Makefile が自動で付与） |

`USLEEPWIN_STATIC` を定義すると `__declspec(dllimport)` が付かなくなります。
定義し忘れると、リンカが `__imp_` 付きシンボルを探して未解決になります。
pkg-config を使う場合は `usleep_win-static.pc` が `-DUSLEEPWIN_STATIC` を出すので、
`pkg-config --cflags usleep_win-static` の結果をそのまま渡せば定義漏れは起きません。

> **⚠ 静的リンクでは `DllMain` が呼ばれません。**
> DLL 版では `DLL_THREAD_DETACH` で `t_timer`（WaitableTimer ハンドル）が、
> `DLL_PROCESS_DETACH` でシステムタイマー分解能が自動的に後始末されますが、
> 静的リンクではこの保険が **一切効きません**。
> `usleep_init_timer_resolution()` / `usleep_init_nt_resolution()` を呼んだら、
> 終了前に必ず `usleep_shutdown_timer_resolution()` / `usleep_shutdown_nt_resolution()` を
> 明示的に呼んでください。呼び忘れると、プロセス終了後もシステム全体のタイマー分解能が
> 上がったまま残ります。

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

> **⚠ `yield_policy` が効くのは「スピン経路に入った待機」だけです。**
> `do_sleep_us()` が WaitableTimer 経路に入ると、タイマー復帰後の末尾スピンは
> `spin_with_yield_until_us(target, 0, USLP_YIELD_NONE)` として呼ばれるため、
> **`yield_policy` は一切参照されません**。経路の選択に効くのは
> **待機長と高分解能タイマーの可用性だけ**で、`spin_last_us` は経路を変えません。

| 待機の条件（BALANCED の場合） | 経路 | `yield_policy` |
|---|---|---|
| `usec == 0` | `SwitchToThread()` | 参照しない |
| `usec >= 2000`（`timer_first_us`） | WaitableTimer + 純スピン | **参照しない** |
| HR タイマーあり かつ `usec > 200`（`prefer_spin_below`） | WaitableTimer + 純スピン | **参照しない** |
| `usec <= 200`、または HR タイマー非対応環境での短い待機 | スピン経路 | **参照する** |
| `LOW_POWER` のスピン経路 | スピン経路 | 参照しない（`Sleep(1)` 固定） |

> 閾値はプロファイル別です（STRICT は `prefer_spin_below = 500`、LOW_POWER は 0）。
> 高分解能 WaitableTimer が使える現代の Windows（10 1803 以降）では、**BALANCED の 1ms 待機は
> 常にタイマー経路に入るため、`yield_policy` を変えても何も変わりません**。
> 実測（v0.2.2 / 2026-08-26）でも 1ms 周期・BALANCED・`spin_last_us=200` の全 40 ランで
> `timer_used` はほぼ全反復、`yield_switch` / `yield_sleep0` / `yield_sleep1` はいずれも 0 で、
> 4 つの `yield_policy` の差は実行ごとのノイズの範囲でした（[test_result.md](./document/test_result.md)）。
> `yield_policy` を効かせたいときは、**待機長を `prefer_spin_below` 以下にする**（BALANCED なら
> 200µs 以下）か、HR タイマーの無い環境を対象にしてください。
> 実際に効いているかは `usleep_get_stats()` の `waitable_timer_uses` / `yield_*` で確認できます。

> **スピン長の上限**: `usleep_set_spin_last_us()` は `USLEEP_SPIN_LAST_US_MAX`（**10000µs = 10ms**）
> を超える値を受け付けません。超過時は **-1 を返し、設定は変更しません**（成功時は 0）。
> 末尾スピンは 1 コアを 100% 占有する区間なので、Windows の既定タイマ分解能（約 15.6ms）や
> 既定クォンタム（クライアントで約 20〜30ms）を超える長さまで許すと、
> 「タイマの粗さを末尾スピンで隠す」という設計目的から外れ、かえって精度が落ちます。

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

プロファイル別の閾値テーブルと内部設計・チューニング詳細は **[実装仕様書 (specsheet.md)](./document/specsheet.md)**、
ベンチマークの実測値は **[テスト結果 (test_result.md)](./document/test_result.md)** を参照してください。

| プロファイル | ジッタ | CPU | 公平性 | 用途 |
|---|---|---|---|---|
| **BALANCED**（既定） | ★★★ | ★★☆ | ★★★ | 1ms メインループ、ゲーム/サーバー |
| **STRICT** | ★★★★ | ★★★★ | ★★☆ | レイテンシ重視、オーディオ |
| **LOW_POWER** | ★☆☆☆ | ★☆☆☆ | ★★★★ | バックグラウンド、省電力 |

### 測定にもとづくチューニング指針

以下はすべて **1 台での実測**（v0.2.2 / 2026-08-26）にもとづく観察です。数値の出典と全表は
[test_result.md](./document/test_result.md) を参照してください。

測定条件: Intel Core Ultra 9 285K（論理 24 コア）/ Windows 11 Home 10.0.26200 / 電源プラン「バランス」 /
MSVC 19.51 release ビルド / `usleep_until_steady_us()` による締切方式・2000 反復 / プロファイル BALANCED /
バックグラウンドはクリーンではない状態（常用アプリが動作し、システム全体の CPU 使用率 4〜6%）。
CPU% は **論理コア 1 個 = 100%** の定義（`QueryThreadCycleTime` 由来、測定スレッド自身のみ）。表はレンジ表記。

**締切精度に効くのは `spin_last_us`（1ms 周期 / 分解能 1ms / n=3）**

| `spin_last_us` | p50 遅着 (µs) | p95 遅着 (µs) | CPU% (1 コア = 100) |
|---:|---:|---:|---:|
| 200 | 90–131 | 361–419 | 2.87–3.28 |
| 400 | 0 | 119–146 | 12.95–13.71 |
| 600 | 0 | 0 | 44.51–44.69 |

- **精度と CPU のトレードオフは `spin_last_us` の直線上にあります。** p95 遅着をゼロにできますが、
  その代償は論理コア 1 個の 45% 前後です。
- `spin_last_us` を待機長以上（この周期なら 1000µs）に取ると、末尾スピンの余白が取れず
  （内部で `coarse_us = usec` に落ちる）逆に遅着が増えます。**周期より短く取ってください。**

**タイマー分解能を 0.5ms にしても、この構成では締切精度は良くなりません**

- 1ms 周期・`spin_last_us=200`（n=5）では、0.5ms（`usleep_init_nt_resolution(5000)`）にしても
  p50 遅着は 71–130µs → 132–149µs と**むしろ悪化**し、CPU% は 2.63–3.72% → 2.28–2.90% と微減しました。
  **機序は本測定では特定できていません**（使用しているのは
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` で、グローバルなタイマー分解能設定とは独立に動作する
  系統である、という事実は関係しうる）。断定はしないでください。
- 分解能が 1 桁効くのは **`Sleep(1)` を踏む構成だけ**です。200µs 周期・`spin_last_us=0`・
  `USLP_YIELD_SLEEP1`（スピン経路）では、p50 遅着が 1ms 分解能で 7808–7906µs、
  0.5ms 分解能で 710–753µs でした。
- タイマー分解能はシステム全体の共有状態なので、**既定では触らない**現行の挙動を推奨します。

**イールド方針は「CPU を下げる手段」ではありません**

- 200µs 周期・`spin_last_us=0`（＝スピン経路）では、`NONE` / `SWITCH_THREAD` / `SLEEP0` はいずれも
  締切をほぼ守る（p50 / p95 遅着 = 0µs）代わりに CPU% は 99.34–100.44%、
  つまり**論理コア 1 個をほぼ丸ごと使います**。`Sleep(0)` は他スレッドに実行機会を与えるだけで、
  自スレッドは走り続けるためです。
- CPU を手放すのは `SLEEP1` だけ（同条件で 1ms 分解能 0.18–0.27% / 0.5ms 分解能 0.71–0.82%）ですが、`Sleep(1)` の実待機がそのまま
  遅延になり、短い周期には追随できません（上記のとおり 1ms 分解能で p50 約 7.8ms）。

> 上記はいずれも**この 1 台・この設定でのスナップショット**です。Windows はハードリアルタイム OS では
> ないため、別のマシン・電源プラン・負荷では傾向ごと変わりえます。自分の環境で測り直してください。

## 📊 ベンチマーク（CSV 出力）
`tools/bench_usleep_csv.cpp` を使うと、イテレーションごとに遅着/CPU%/譲り回数などを CSV で出力できます。

```bash
# 引数: [iters] [tick_us] [spin_last_us] [yield_policy] [profile] [label]
bench_usleep_csv.exe 2000 1000 200 2 0 balanced_sleep0_spin200 > result.csv
```

出力列（1 行 = 1 反復。`late_us` / `cum_thr_cpu_pct` 以外は `usleep_stats_t` の反復ごとの差分）:
```
iter,late_us,cum_thr_cpu_pct,spin_relax,yield_switch,yield_sleep0,yield_sleep1,timer_used
```

- `late_us` … 実到着 − 締切（早着は 0 に丸め）
- `cum_thr_cpu_pct` … ループ開始からの**累積** CPU%。`QueryThreadCycleTime` のサイクル差分を
  起動時に実測した換算係数（cycles/µs）で時間に直し、同区間の実時間（QPC）で割った値。
  **論理コア 1 個 = 100%**。区間ごとの瞬時値は出しません（1ms 区間では量子化で暴れるため）

末尾には `#SUMMARY` 行群が出ます（**stdout と stderr の両方**に出るので、CSV をリダイレクトしたまま
stderr だけ集計できます）。含まれる項目:

| `#SUMMARY` の内容 | 意味 |
|---|---|
| `label` / `profile` / `yield` / `spin_us` / `tick_us` / `iters` / `ncpu` | 測定条件 |
| `avg_late_us` / `p50_late_us` / `p95_late_us` / `p99_late_us` / `max_late_us` | 遅着の分布 |
| `thread_cycle_cpu_pct_of_1core` | **主値**。`QueryThreadCycleTime` 由来 |
| `thread_cpu_pct_of_1core` | 参考値。`GetThreadTimes` 由来（ティックサンプリング課金） |
| `process_cpu_pct_of_1core` / `process_cpu_pct_of_system` | プロセス全スレッド合算 |
| `cycles_per_us` / `thread_cycles` / `thread_cycle_cpu_us` | 換算係数と生のサイクル数 |
| `wall_total_us` / `thread_cpu_us` / `cpu_us_per_iter` / `ns_per_spin_relax` | 妥当性チェック用の派生量 |
| `nt_resolution_100ns min= max= cur= cur_end= query_ok= requested= acquired=` | タイマー分解能の測定条件と、**測定中に他プロセスに奪われていないか**（`cur != cur_end` の run は捨てる） |
| `spin_relax` / `yield_switch` / `yield_sleep0` / `yield_sleep1` / `timer_used` | 実際に踏んだ待機経路の内訳 |

環境変数 `USLEEP_BENCH_NT_RES_100NS` でタイマー分解能を明示できます（未設定なら**一切触らず**、
他プロセス任せの素の状態を測ります）。`max` で環境の最も細かい値、数値なら 100ns 単位（`5000` = 0.5ms）。
測定後は自動で `usleep_shutdown_nt_resolution()` されます。

> **経路を確認せずに数値を読まないでください。** `timer_used` / `yield_*` の内訳を見ないと、
> 「何を測っているつもりか」と「実際に走った経路」がずれていても気付けません。

CPU% の計器は `tools/cpu_accounting.h` に集約されており、`tests/test_usleep.cpp` の
`test_cpu_accounting()` が**同じヘッダ**を使って「純ビジー待機は論理コア 1 個の 100% 付近」
「寝ているだけのスレッドは 0%」を回帰テストしています。計器が壊れたらベンチではなくテストが落ちます。

ベンチマーク実測結果は、測定条件および v0.2.2 以前の CPU 数値を撤回した理由とあわせて
**[テスト結果 (test_result.md)](./document/test_result.md)** に記載しています。Windows はハードリアルタイム OS では
ないため、そこに載る数値も 1 台・1 設定でのスナップショットであり、保証ではありません。

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
 ├ bench_usleep_csv.cpp   … CSV ベンチ
 ├ cpu_accounting.h       … CPU 使用率の計器（ベンチとテストで共有するヘッダ）
 └ meson_build_msvc.ps1   … MSVC 用ビルドラッパー
tests/
 └ test_usleep.cpp
.github/workflows/
 └ ci.yml
```

`tools/cpu_accounting.h` はビルドシステムに登録しない純粋なヘッダで、ベンチとテストの両方から
include されます。「ベンチが表示する CPU% とテストが検証する CPU% が同じ計器である」ことを
コンパイル時に保証するためです。

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
