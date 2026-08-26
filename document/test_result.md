# usleep_win — テスト結果

> 注: Windows はハードリアルタイム OS ではない。本ページの数値はいずれも特定の 1 台・特定の設定での
> スナップショットであり、精度を保証するものではない。締切を守ることが安全性に直結する用途には使わないこと。

このページには 2 世代の測定結果が載っている。**新しい方（v0.2.2 / 2026-08-26）だけが現行の実測値**である。
古い方（v0.2.0 / 2026-03-11）は、後述のとおり **CPU 使用率の計器が壊れていた**ため参考記録として残してある。
比較のために消さずに置いてあるだけで、数値を引用してはならない。

---

# v0.2.2 再測定（2026-08-26）

## 測定環境

| 項目 | 値 |
|---|---|
| OS | Windows 11 Home 10.0.26200 |
| CPU | Intel Core Ultra 9 285K（物理 24 / 論理 24、公称最大 3.7GHz） |
| 電源プラン | バランス（GUID `381b4222-f694-41f0-9685-ff5bb260df2e`） |
| コンパイラ | MSVC 19.51.36252 (x64) |
| ビルド | `meson setup --buildtype=release -Dwerror=true` → `meson compile`（`/O2`、`-Dnative=false`） |
| バージョン | v0.2.2 |
| NT タイマー分解能 | min（最も粗い）=15.625ms / max（最も細かい）=0.5ms / 測定開始時の cur=**1.0ms** |
| バックグラウンド負荷 | **クリーンな環境ではない**。VS Code とエージェントセッションが常駐し、システム全体の CPU 使用率は 4〜6%（24 論理コア基準）。単一スレッドのベンチなのでコアの奪い合いは起きにくいが、外れ値（max_late）はこの負荷の影響を受けている |

- **DLL とベンチ実行ファイルは同一ツリーから同時にビルドしたペアを使用**した（stale なバイナリ混入を防ぐため、`build-fresh-msvc` を作り直してから測定）。
- 測定開始時の `cur=1.0ms` は、本プロセスが要求した値ではなく**他プロセスが `timeBeginPeriod` 等で握っている値**である。これが Windows デスクトップの実情なので、これを「既定状態」条件とした。

## 測定条件（2 条件 × 各 n=5）

| 条件名 | タイマー分解能 | 設定方法 |
|---|---|---|
| **既定（1ms）** | cur = 1.0ms | 本プロセスからは一切設定しない（他プロセス任せ） |
| **明示（0.5ms）** | cur = 0.5ms | `usleep_init_nt_resolution(5000)`（= NtSetTimerResolution 0.5ms）。測定後 `usleep_shutdown_nt_resolution()` で解除 |

分解能はプロセス横断の共有状態なので、ベンチは要求値と**測定開始時 / 終了時の実効値**を `#SUMMARY nt_resolution_100ns` 行に必ず出力する。上記の全ランで `cur == cur_end == 要求値`、つまり測定中に他プロセスに奪われていないことを確認済み。

各条件・各 config で **n=5 回**反復し、以下の表は **最小〜最大のレンジ**で記載する。単一値は出さない。

## CPU 使用率の計器について（旧数値を信用してはいけない理由）

旧ベンチの `cpu_pct` は `GetProcessTimes` の差分だった。この API はスケジューラのクロックティック
（既定 15.625ms）ごとの**サンプリング課金**で、「ティック割り込みの瞬間に走っていたスレッド」に
1 ティック分をまとめて課金する。1ms 周期で数百 µs だけ走る本ベンチの負荷型では、

- ティックと位相が合わなければ 0% 近くに出る
- 位相が合えば桁で過大に出る

という二値的な振る舞いになる。実際、旧ベンチの CSV は 3000 行中 2995 行が `0.00`、5 行が `1562.50` で、
同一条件を 4 回回すと ±45% ばらついた。**`document/README_en.md` の 0.02% も、下の旧表の 48.5% も、
同じ計器のノイズの別の顔である。**

現行ベンチは `QueryThreadCycleTime`（コンテキストスイッチごとにサイクルカウンタ差分を積算するので
サンプリング課金の問題が無い）を、起動時に実測した換算係数（cycles/µs）で時間に直して使う。
本測定での再現性は、同一条件 n=5 のレンジで見て 2.86–3.60%（1ms・NONE）のように ±13% 程度に収まる。
正規化の裏取りとして、純ビジー待機（1µs 締切を 300ms 連続）が 99.91 / 99.98 / 100.24% を返すことを
3 ビルド（MSVC 共有 / MSVC 静的 / MinGW）で確認した。

この「純ビジー待機 ≒ 論理コア 1 個の 100%」というアンカーは、**単体テストの回帰テストとして固定した**
（`tests/test_usleep.cpp` の `test_cpu_accounting()`。上下両方の境界と、寝ているだけのスレッドが 0% を
返すことの両方をアサートする）。計器が再び壊れたらベンチではなくテストが落ちる。

---

## 単体テスト結果（v0.2.2 / MSVC release / CI モード off）

```
[TEST] usleep_win test suite (CI mode: off)
[TEST] wait-path proof via stats counters...
    [stats] usleep_win(0)              relax=0 switch=1 sleep0=0 sleep1=0 timer=0
    [stats] nsleep_win(999ns)          relax=0 switch=1 sleep0=0 sleep1=0 timer=0
    [stats] 100 x nsleep_win(1000ns)   relax=3043 switch=0 sleep0=0 sleep1=0 timer=0
    [stats] BALANCED spin=200 x 1us    relax=61420 switch=0 sleep0=0 sleep1=0 timer=0
    [stats] BALANCED spin=0 SLEEP0     relax=11115 switch=0 sleep0=58 sleep1=0 timer=0
    [stats] STRICT spin=0 SWITCH       relax=28968 switch=153 sleep0=0 sleep1=0 timer=0
    [stats] BALANCED spin=0 SLEEP1     relax=189 switch=0 sleep0=0 sleep1=1 timer=0
    [stats] BALANCED spin=0 NONE       relax=8848 switch=0 sleep0=0 sleep1=0 timer=0
    [stats] BALANCED 2000us            relax=0 switch=0 sleep0=0 sleep1=0 timer=1
    [stats] BALANCED 201us             relax=0 switch=0 sleep0=0 sleep1=0 timer=1
    high-resolution waitable timer: available
    [stats] LOW_POWER 1000us           relax=0 switch=0 sleep0=0 sleep1=0 timer=1
[TEST] wait duration statistics (lower bound + p95)...
    usleep_win(1us)      min=1 p50=1 p95=1 max=1 (us)
    usleep_win(100us)    min=100 p50=100 p95=100 max=100 (us)
    usleep_win(1ms)      min=1000 p50=1020 p95=1152 max=1228 (us)
    usleep_win(20ms)     min=20016 p50=20145 p95=20328 max=20328 (us)
    usleep_win(1s)       min=1000341 p50=1000341 p95=1000341 max=1000341 (us)
[TEST] deadline scheduling...
    lateness p50=104 p95=348 max=475 us / total=50270 us
    [stats] until_steady_us(past)      relax=0 switch=0 sleep0=0 sleep1=0 timer=0
[TEST] Sleep(0) yield actually lets a peer thread run...
    [stats] 2000 x 200us (SLEEP0)      relax=22959393 switch=0 sleep0=120610 sleep1=0 timer=0
    elapsed=400433 us, worker ticks=107392537
[TEST] setter validation...
    [stats] spin_last_us reject keeps 0 relax=8814 switch=0 sleep0=46 sleep1=0 timer=0
    [stats] set_profile overwrite      relax=6432 switch=0 sleep0=0 sleep1=0 timer=0
[TEST] timer resolution round-trip...
    NT resolution: min=156250 max=5000 cur=10000 (100ns)
    after init:     cur=5000 (100ns)
    after shutdown: cur=10000 (100ns)
[TEST] per-thread config / stats isolation...
    main: sleep0=11751 switch=0 | worker: sleep0=0 switch=30783
[TEST] forced wait backends (unreachable-on-modern-Windows paths)...
    [stats] LOW_POWER 500us (no HR)    relax=189 switch=0 sleep0=0 sleep1=1 timer=0
    [stats] BALANCED 201us (no HR)     relax=189 switch=0 sleep0=1 sleep1=0 timer=0
    [stats] LOW_POWER 2500us (no timer) relax=316 switch=0 sleep0=0 sleep1=20 timer=0
    Sleep(ms) fallback: min=2500 p50=2999 max=3020 us
    [stats] BALANCED 3000us (auto)     relax=0 switch=0 sleep0=0 sleep1=0 timer=1
    worker timer_uses while main forced NO_TIMER: 1
[TEST] CPU accounting meter (regression guard for the bench)...
    calibrated: 3664.1 cycles/us
    [stats] busy 1us x 300ms           relax=16367904 switch=0 sleep0=0 sleep1=0 timer=0
    busy: cycle=100.24% getthreadtimes=98.96% of 1 core (wall=300000 us)
    idle: cycle=0.00% of 1 core (wall=209587 us)
[OK] all tests passed.
```

共有版・静的版（`USLEEPWIN_STATIC`）の両方、および MSVC 19.51 / MinGW g++ 16.2.0 の両方で PASS。

---

## ベンチマーク結果

共通条件: `usleep_until_steady_us()` による締切方式、iters=2000、プロファイル `USLP_BALANCED`。
CPU% は **論理コア 1 個 = 100%** の定義（`QueryThreadCycleTime` 由来。測定スレッド自身のみ）。

### グループA — 旧表と同条件（1ms 周期 / 末尾スピン 200µs）

n=5 のレンジ。`timer` 以外の経路カウンタは全ランで 0。

**既定（分解能 1ms）**

| config | yield_policy | spin_us | p50_late (µs) | p95_late (µs) | p99_late (µs) | max_late (µs) | CPU% (1コア=100) |
|---|---|---:|---:|---:|---:|---:|---:|
| balanced_none_spin200 | NONE | 200 | 94–121 | 317–397 | 484–637 | 903–1340 | 2.86–3.60 |
| balanced_switch_spin200 | SWITCH_THREAD | 200 | 71–125 | 306–380 | 426–609 | 762–1554 | 2.72–3.72 |
| balanced_sleep0_spin200 | SLEEP0 | 200 | 81–130 | 314–335 | 449–519 | 707–1603 | 2.92–3.52 |
| balanced_sleep1_spin200 | SLEEP1 | 200 | 77–115 | 322–380 | 483–664 | 798–1595 | 2.63–3.36 |

**明示（分解能 0.5ms）**

| config | yield_policy | spin_us | p50_late (µs) | p95_late (µs) | p99_late (µs) | max_late (µs) | CPU% (1コア=100) |
|---|---|---:|---:|---:|---:|---:|---:|
| balanced_none_spin200 | NONE | 200 | 133–146 | 357–389 | 449–476 | 589–830 | 2.36–2.90 |
| balanced_switch_spin200 | SWITCH_THREAD | 200 | 138–149 | 357–388 | 448–489 | 660–1018 | 2.28–2.66 |
| balanced_sleep0_spin200 | SLEEP0 | 200 | 132–144 | 352–382 | 430–466 | 541–904 | 2.32–2.77 |
| balanced_sleep1_spin200 | SLEEP1 | 200 | 133–145 | 338–386 | 410–483 | 535–1077 | 2.36–2.84 |

> **重要（この 4 行は同じコードを測っている）**
> 全 40 ランで `timer_used = 1994〜2000`（ほぼ全反復）、`yield_switch = yield_sleep0 = yield_sleep1 = 0`。
> 1ms 周期 × BALANCED（`prefer_spin_below=200`）では **必ず WaitableTimer 経路に入り**、タイマー後の
> 末尾スピンは実装上 `spin_with_yield_until_us(target, 0, USLP_YIELD_NONE)` 固定で、
> **`yield_policy` は一切参照されない**。
> つまり旧表の「yield_policy 別の 4 行」は v0.2.2 では**同一経路の再測定 4 本**であり、行間の差は
> 実行ごとのノイズにすぎない。旧表で SLEEP1 だけ avg_late 7491µs と突出していたのは v0.2.0 の
> 経路構成に由来するもので、現行実装では再現しない。
> **yield_policy の比較をしたければ、待機長を `prefer_spin_below` 以下にしてスピン経路に入れる
> 必要がある**（次のグループB）。

### グループB — yield_policy が実際に効く条件（200µs 周期 / spin_last_us=0）

200µs は BALANCED の `prefer_spin_below` と等しいのでタイマー経路に入らず、`spin_last_us=0` により
毎回イールドが発行される。経路はカウンタで確認済み（`timer_used=0`、該当イールドカウンタのみ増加）。

**既定（分解能 1ms）**

| config | yield_policy | p50_late (µs) | p95_late (µs) | max_late (µs) | CPU% (1コア=100) | 該当イールド回数 |
|---|---|---:|---:|---:|---:|---:|
| spin200us_none | NONE | 0 | 0 | 16–215 | 99.78–100.44 | —（純スピン） |
| spin200us_switch | SWITCH_THREAD | 0 | 0 | 63–115 | 99.38–99.75 | switch 121340–123629 |
| spin200us_sleep0 | SLEEP0 | 0 | 0 | 70–120 | 99.36–99.51 | sleep0 122277–123885 |
| spin200us_sleep1 | SLEEP1 | 7808–7906 | 14744–14895 | 15964–16067 | 0.18–0.27 | sleep1 26–27 |

**明示（分解能 0.5ms）**

| config | yield_policy | p50_late (µs) | p95_late (µs) | max_late (µs) | CPU% (1コア=100) | 該当イールド回数 |
|---|---|---:|---:|---:|---:|---:|
| spin200us_none | NONE | 0 | 0 | 20–308 | 99.64–100.30 | —（純スピン） |
| spin200us_switch | SWITCH_THREAD | 0 | 0 | 83–148 | 99.34–99.74 | switch 122076–124042 |
| spin200us_sleep0 | SLEEP0 | 0 | 0 | 89–123 | 99.53–99.75 | sleep0 122218–123904 |
| spin200us_sleep1 | SLEEP1 | 710–753 | 1402–1439 | 1972–4049 | 0.71–0.82 | sleep1 267–280 |

- NONE / SWITCH_THREAD / SLEEP0 は 200µs 周期の締切をほぼ完全に守るが、**論理コア 1 個をほぼ丸ごと使う**（≒100%）。
  「SLEEP0 なら CPU が下がる」ということはない。`Sleep(0)` は他スレッドに実行機会を与えるだけで、
  自スレッドは走り続けるからである（ワーカースレッドが進むことは単体テストで別途確認している）。
- SLEEP1 だけが CPU を手放すが、`Sleep(1)` の実待機時間が分解能に直結するため 200µs 周期には全く追随できない。
  **ここが分解能依存の最も大きい箇所**で、1ms → 0.5ms で p50 遅延が 7.8ms → 0.71ms と 1 桁改善する
  （同時に CPU% は 0.2% → 0.8% に上がる）。

### グループC — `spin_last_us` スイープ（1ms 周期 / SLEEP0 / n=3）

分解能依存の正体を切り分けるための追加測定。

| spin_last_us | 分解能 | p50_late (µs) | p95_late (µs) | max_late (µs) | CPU% | spin_relax/反復 |
|---:|---|---:|---:|---:|---:|---:|
| 200 ※ | 1ms | 90–131 | 361–419 | 833–1492 | 2.87–3.28 | 1185–1389 |
| 400 | 1ms | 0 | 119–146 | 569–1210 | 12.95–13.71 | 6832–7285 |
| 600 | 1ms | 0 | 0 | 305–1083 | 44.51–44.69 | 25537–25733 |
| 1000 | 1ms | 276–296 | 519–538 | 994–1233 | 0.58 | 9–34 |
| 200 ※ | 0.5ms | 155–157 | 344–350 | 521–1429 | 3.02–3.39 | 1242–1389 |
| 400 | 0.5ms | 41–45 | 196–216 | 457–587 | 9.55–12.29 | 5361–6632 |
| 600 | 0.5ms | 0 | 0 | 30–217 | 47.67–47.87 | 28340–28616 |
| 1000 | 0.5ms | 283–302 | 536–594 | 879–1015 | 0.57–0.69 | 2–7 |

※ この 200 は `spin_last_us` の**既定値ではない**（既定は 250）。BALANCED の内部閾値
`kProfileThresholds[BALANCED].prefer_spin_below` が 200 であり、それと同値になるよう
グループA/B と条件を揃えて**明示指定**したもの。

`spin_last_us=1000` の行は `usec > spin_last_us` が偽になり `coarse_us = usec`（末尾スピンの余白を取らない）
に落ちるケース。末尾スピンがほぼ発生せず（relax/反復が 1 桁）、タイマー起床の遅れがそのまま遅延になる。

---

## 知見: タイマー分解能と「踏まれない末尾スピン」

1ms 周期・末尾スピン 200µs の設定では、**タイマーが締切を過ぎてから起きる反復が多数派**であり、
その反復では末尾スピンが 1 回も回らない。1 反復ごとの `spin_relax` 差分を数えると:

| 分解能 | `spin_relax == 0` の反復（末尾スピンを踏まなかった） | `late == 0` の反復（締切前に起床） | late p50 / p95 |
|---|---:|---:|---:|
| 1ms（既定） | 1346 / 2000（**67.3%**） | 619 / 2000（30.9%） | 101 / 385 µs |
| 0.5ms（明示） | 1667 / 2000（**83.3%**） | 317 / 2000（15.8%） | 132 / 397 µs |

つまり **200µs の末尾スピンは、この周期では 2/3 以上の反復で「死んだコード」になっている**。
締切を守れているように見える平均値の裏で、実際には「タイマー起床が間に合った反復だけ」が
スピンで詰められている。

**当初の仮説は測定で否定された。** 「0.5ms にすれば末尾スピンが踏まれるようになり CPU% が上がるはず」
と予想していたが、実測では逆で、0.5ms の方が末尾スピンを踏む反復が減り（67.3% → 83.3% が未実行）、
p50 遅延はむしろ悪化（101µs → 132µs）、CPU% は微減（2.9±0.4% → 2.5±0.3%）した。
この差は n=5 のレンジが重ならない程度には安定して再現する。

因果は本測定では確定できていない。留意すべき事実としては、

- 使っているのは `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` の高分解能タイマーで、Windows 10 2004 以降は
  グローバルなタイマー分解能設定とは独立に動作する系統である。したがって「分解能を上げれば
  WaitableTimer が正確になる」という単純な関係は成り立たない。
- `NtSetTimerResolution(0.5ms)` は割り込み頻度を上げるため、システム全体の挙動（省電力状態遷移を含む）
  が変わる。CPU% がむしろ下がっていることと整合する説明が付く可能性はあるが、本測定では検証していない。

**実務上の結論**（この 1 台での話であることに注意）:

- 1ms 周期で締切を確実に守りたいなら、効くのは分解能ではなく **`spin_last_us` を増やすこと**。
  600µs で p95 遅延がゼロになるが、CPU は 45〜48%（論理コア 1 個基準）まで上がる。**精度と CPU の
  トレードオフは `spin_last_us` の直線上にある**（200µs→3%、400µs→13%、600µs→45%）。
- 分解能を 0.5ms に固定して得があるのは、`Sleep(1)` を踏む構成（LOW_POWER / SLEEP1）だけである。
  そこでは 1 桁効く（グループB）。
- タイマー分解能はシステム全体の共有状態なので、ライブラリ側で勝手に握らない現行の既定
  （`init` を呼ばない限り触らない）は妥当。

---

# 参考記録: v0.2.0 の旧測定（2026-03-11）— **引用禁止**

> 以下は歴史的記録として残す。**CPU 使用率の列は計器が壊れており（上記「CPU 使用率の計器について」）、
> 数値としての意味を持たない。** 遅延の列も、測定機（AMD Ryzen 7 5800H）・電源プラン・分解能・
> バックグラウンド負荷がいずれも未記録で、v0.2.2 の測定機（Intel Core Ultra 9 285K）とは別マシンである。
> v0.2.2 の表と直接比較してはならない。

## 環境（当時）

- OS: Windows 10.0.26200
- CPU: AMD Ryzen 7 5800H with Radeon Graphics
- コンパイラ: MSVC 14.50 (Visual Studio 18.3.1)
- バージョン: v0.2.0
- 電源プラン / タイマー分解能設定 / 同時実行していた負荷: **記録なし**

## 単体テスト結果（当時）

```
[TEST] usleep_win basic timing...
  usleep_win(200us) -> 200 us
  usleep_win(2ms)   -> 2414 us
  usleep_win(20ms)  -> 20237 us
[TEST] deadline scheduling (drift check)...
  max late: 428 us
[TEST] yield behavior with Sleep(0)...
  worker ticks = 2218149
[OK] all tests passed.
```

> このときの「イールド動作 (Sleep(0))」テストは、後に**空振り**であったことが判明している
> （BALANCED + `spin_last_us=200` + 短い待機では `remain <= spin_last_us` となり `Sleep(0)` が
> 1 回も発行されない）。現在は `tests/test_usleep.cpp` に回帰ガードとして固定してある。

## ベンチマーク結果（当時 / 2000 反復 / 1ms 周期 / 末尾スピン 200µs）

| config | yield_policy | spin_us | avg_late (µs) | p95_late (µs) | p99_late (µs) | max_late (µs) | avg_cpu (%) |
|---|---|---:|---:|---:|---:|---:|---:|
| balanced_none_spin200 | NONE | 200 | 0.33 | 1 | 1 | 115 | 100.0 |
| balanced_switch_spin200 | SWITCH_THREAD | 200 | 0.39 | 1 | 5 | 94 | 97.7 |
| balanced_sleep0_spin200 | SLEEP0 | 200 | 0.90 | 1 | 1 | 941 | 96.1 |
| balanced_sleep1_spin200 | SLEEP1 | 200 | 7491.14 | 14395 | 15119 | 15544 | 48.5 |

> `avg_cpu` 列は `GetProcessTimes` のティック量子化の産物であり、値としては無意味。
> また p95_late = 1µs という値は、当時のプロファイル閾値では 1ms 待機がタイマー経路に入らず
> 純スピンしていたことを示している（v0.2.1 の `probe_hrtimer_support()` 化で経路が変わった）。

## リファクタリング内容（v0.2.0 時点の記録）

| ID | 内容 | 影響 |
|---|---|---|
| R1 | `qpc_now_us()`: `long double` → 純粋整数演算 | MSVC の `long double == double` 問題を解消 |
| R2 | `usleep_set_power_mode()`: `GetProcAddress` のキャッシュ化 | 関数ポインタの取得を初回のみに |
| R3 | `do_sleep_us()`: プロファイル閾値の `constexpr` テーブル化 | マジックナンバー排除 |
| R4 | `do_sleep_us()`: タイマー後スピンパス統合 | 重複コードパス削除 |
| R5 | `DllMain`: シャットダウンロジックを内部ヘルパーに抽出 | 重複排除 |

> **補足（v0.2.1 で判明）**: R2 の時点では `THREAD_INFORMATION_CLASS` の `ThreadPowerThrottling` を
> 誤って 11 と定義していたため、`usleep_set_power_mode()` は全モードで -1 を返しており、
> **当時の測定はいずれも電力モード未適用の状態**である。定数は v0.2.1 で 3 に修正された。

---

## 再測定の手順（次回の再現用）

```bash
# 1. 同一ツリーから DLL とベンチを作り直す（stale なバイナリを混ぜない）
meson setup build-fresh-msvc --buildtype=release -Dwerror=true
meson compile -C build-fresh-msvc

# 2. 既定（他プロセス任せ）の分解能で測る
cd build-fresh-msvc
./bench_usleep_csv.exe 2000 1000 200 2 0 balanced_sleep0_spin200 > out.csv

# 3. 0.5ms を明示的に握って測る（測定後に自動で解除される）
USLEEP_BENCH_NT_RES_100NS=5000 ./bench_usleep_csv.exe 2000 1000 200 2 0 balanced_sleep0_spin200 > out.csv
```

- サマリは stdout（CSV のコメント行）と stderr の両方に出るので、CSV をリダイレクトしたまま
  stderr だけ集計できる。
- `#SUMMARY nt_resolution_100ns ... cur=... cur_end=...` を必ず確認すること。
  `cur != cur_end` なら測定中に他プロセスが分解能を変えており、その run は捨てる。
- 経路を確認せずに数値を読まないこと。`timer_used` / `yield_*` の内訳を見ないと、
  「何を測っているつもりか」と「実際に走った経路」がずれていても気付けない（グループA の教訓）。
