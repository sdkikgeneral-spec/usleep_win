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

> 注: Windows はハードリアルタイム OS ではありません。ミクロな待機は環境差（電源管理、仮想化、セキュリティソフト等）の影響を受けます。

---

## ✨ 特長

### 🔧 高精度・低ジッタ
- High-Resolution Waitable Timer が利用可能な環境では **100ns 分解能**
- 最後の **200–400µs** だけスピンで詰め、遅着を抑制しやすい

### 🧵 CPU に優しい待機
- スピン中に `YieldProcessor()` を発行
- **64 回に 1 回** `Sleep(0)` / `SwitchToThread()` を挟み、公平性と発熱のバランスを確保
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

---

## 📦 ビルド & インストール

### Meson（推奨）
```bash
meson setup build --buildtype=release
meson compile -C build
```

### MinGW (Makefile)
```bash
mingw32-make
```

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

### 5) **統計カウンタの読み取り（ベンチ用）**
```cpp
usleep_stats_t st{};
usleep_get_stats(&st);

printf("spin_relax=%llu\n", (unsigned long long)st.spin_relax);
printf("yield_sleep0=%llu\n", (unsigned long long)st.yield_sleep0);
```

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

#### スピン/イールド微調整のコツ
- `usleep_set_spin_last_us(200〜400)` : 長く→ジッタ↓/CPU↑、短く→CPU↓/ジッタ↑
- `usleep_set_yield_policy(...)` : `SLEEP0`（既定）、`SWITCH_THREAD`（局所性）、`SLEEP1`（省電力）

---

## 📊 ベンチマーク（CSV 出力）
`tools/bench_usleep_csv.cpp` を使うと、イテレーションごとに遅着/CPU%/譲り回数などを CSV で出力できます。

```bash
bench_usleep_csv.exe 2000 1000 200 2 > result.csv
```

出力列:
```
iter,late_us,cpu_pct,spin_relax,yield_switch,yield_sleep0,yield_sleep1,timer_used
```

### 実測比較（2026-03-07）

以下は同一環境で `2000 iter / 1000us tick` を 3 回ずつ実行し、各指標を平均した結果です。

- OS: Windows 10.0.26200.7922
- CPU: AMD Ryzen 7 5800H with Radeon Graphics
- プロファイル: `USLP_BALANCED` 固定
- 集計元: `bench_outputs/summary_runs.csv` / `bench_outputs/summary_agg.csv`

| config | 設定 | avg_late_us | p95_late_us | p99_late_us | max_late_us | avg_cpu_pct |
|---|---|---:|---:|---:|---:|---:|
| balanced_none_spin200 | `yield=NONE, spin=200` | 0.14 | 1.00 | 1.00 | 57.67 | 99.96 |
| balanced_sleep0_spin200 | `yield=SLEEP0, spin=200` | 0.25 | 0.67 | 1.00 | 115.33 | 99.46 |
| balanced_sleep0_spin300 | `yield=SLEEP0, spin=300` | 0.29 | 1.00 | 1.00 | 132.33 | 99.43 |
| balanced_switch_spin200 | `yield=SWITCH_THREAD, spin=200` | 0.31 | 0.33 | 1.00 | 203.33 | 98.90 |
| balanced_sleep1_spin200 | `yield=SLEEP1, spin=200` | 7505.87 | 14403.00 | 15150.00 | 15876.33 | 0.02 |

補足:
- `SLEEP1` は CPU 使用率を大幅に下げる代わりに、1ms 周期用途では遅延が大きくなる傾向です。
- `NONE/SLEEP0/SWITCH_THREAD` はいずれも低遅延ですが、CPU 使用率は高めになります。
- 本ベンチでは `timer_used=0` で、waitable timer ではなく主にスピン/譲りで収束していることを示しています。

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
