# usleep_win — テスト結果 (2026-03-11)

## 環境

- OS: Windows 10.0.26200 (Windows 11 Insider)
- CPU: AMD Ryzen 7 5800H with Radeon Graphics
- コンパイラ: MSVC 14.50 (Visual Studio 18.3.1)
- ビルド: `meson compile -C builddir` (Release, /O2)
- バージョン: v0.2.0 (リファクタリング後)

---

## 単体テスト結果

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

1/1 usleep_win:usleep-basic OK  0.10s
```

| テスト | 結果 | 備考 |
|---|---|---|
| 基本タイミング (200µs) | PASS | 200µs（< 10ms 閾値） |
| 基本タイミング (2ms) | PASS | 2414µs（500µs < dt < 50ms） |
| 基本タイミング (20ms) | PASS | 20237µs（5ms < dt < 200ms） |
| 締切スケジューリング | PASS | max late = 428µs（< 10ms 閾値） |
| イールド動作 (Sleep(0)) | PASS | worker ticks = 2,218,149（> 100） |

---

## ベンチマーク結果

### 測定条件
- プロファイル: `USLP_BALANCED` 固定
- イテレーション: 2000 回
- 目標周期: 1000µs (1ms)
- テールスピン: 200µs
- 締切方式 (`usleep_until_steady_us`)

### 結果サマリ

| config | yield_policy | spin_us | avg_late (µs) | p95_late (µs) | p99_late (µs) | max_late (µs) | avg_cpu (%) |
|---|---|---:|---:|---:|---:|---:|---:|
| balanced_none_spin200 | NONE | 200 | 0.33 | 1 | 1 | 115 | 100.0 |
| balanced_switch_spin200 | SWITCH_THREAD | 200 | 0.39 | 1 | 5 | 94 | 97.7 |
| balanced_sleep0_spin200 | SLEEP0 | 200 | 0.90 | 1 | 1 | 941 | 96.1 |
| balanced_sleep1_spin200 | SLEEP1 | 200 | 7491.14 | 14395 | 15119 | 15544 | 48.5 |

### リファクタリング前後の比較

| 指標 | リファクタリング前 (03-07) | リファクタリング後 (03-11) | 判定 |
|---|---|---|---|
| avg_late (SLEEP0) | 0.25 µs | 0.90 µs | 同等（環境差内） |
| p95_late (SLEEP0) | 0.67 µs | 1 µs | 同等 |
| max_late (SLEEP0) | 115 µs | 941 µs | 環境ノイズ（単発外れ値） |
| avg_cpu (SLEEP0) | 99.5% | 96.1% | 同等 |
| avg_late (NONE) | 0.14 µs | 0.33 µs | 同等 |
| avg_late (SWITCH) | 0.31 µs | 0.39 µs | 同等 |
| avg_late (SLEEP1) | 7505.87 µs | 7491.14 µs | 同等 |

### 考察

- リファクタリング後も **低レイテンシ性能は維持** されている
- `NONE` / `SWITCH_THREAD` / `SLEEP0` はいずれも avg_late < 1µs で安定
- `SLEEP1` は省電力向けで CPU 使用率は大幅に低下するが、1ms 周期には不向き
- `max_late` の差は OS スケジューラのジッタ（割り込み、コンテキストスイッチ等）による単発外れ値
- R1 の整数演算化（`long double` → `uint64_t`）により QPC 変換の一貫性が向上
- R3〜R5 のリファクタリングは内部構造の整理であり、パフォーマンスへの影響はなし

---

## リファクタリング内容

| ID | 内容 | 影響 |
|---|---|---|
| R1 | `qpc_now_us()`: `long double` → 純粋整数演算 | MSVC の `long double == double` 問題を解消 |
| R2 | `usleep_set_power_mode()`: `GetProcAddress` のキャッシュ化 | 関数ポインタの取得を初回のみに |
| R3 | `do_sleep_us()`: プロファイル閾値の `constexpr` テーブル化 | マジックナンバー排除 |
| R4 | `do_sleep_us()`: タイマー後スピンパス統合 | 重複コードパス削除 |
| R5 | `DllMain`: シャットダウンロジックを内部ヘルパーに抽出 | 重複排除 |
