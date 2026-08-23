# usleep_win

Windows 上で Linux の `usleep()` 相当の待機を「実用的な精度・低負荷」で提供する、依存関係ゼロの小型 C/C++ DLL。

## 全体像

単一の実装ファイル `src/usleep_ex.cpp` と単一の公開ヘッダ `include/usleep_win.h` で構成される。公開 API はすべて `extern "C"`。

待機は **ハイブリッド方式**:

```
do_sleep_us(usec)
  ├─ usec == 0                              → SwitchToThread()
  ├─ usec >= timer_first_us                 → WaitableTimer で粗く待つ + 末尾スピン
  ├─ HR タイマーあり && usec > prefer_spin_below → 同上
  ├─ LOW_POWER                              → spin_with_yield_until_us(..., SLEEP1)
  └─ それ以外                                → spin_with_yield_until_us(..., yield_policy)
```

- 時刻源は `QueryPerformanceCounter`。`qpc_now_us()` は商・剰余に分けた **純整数演算**（浮動小数点を使わない）。
- 末尾の数百 µs だけスピンで詰め、スピン中は `YieldProcessor()`（x86: PAUSE / ARM64: YIELD）を発行、64 回に 1 回だけイールドを挟む。
- 閾値は `kProfileThresholds[]` にプロファイル別のテーブルとして集約されている。**閾値を変えるときはこのテーブルだけを触ること。**

## 状態のスコープ — 最重要

この DLL の状態は 2 種類あり、混同するとバグになる。

| 状態 | スコープ | 該当 |
|---|---|---|
| `t_cfg`（profile / spin_last_us / yield_policy / power_mode） | **スレッドローカル** | `usleep_set_*` 系 |
| `t_stat_*`（統計カウンタ） | **スレッドローカル** | `usleep_get_stats` / `usleep_reset_stats` |
| `t_timer`（WaitableTimer ハンドル） | **スレッドローカル** | `get_timer_handle()` |
| `g_time_period_ms` / `g_nt_resolution_100ns` / `g_has_hrtimer` | **プロセス全体**（`std::atomic`） | `usleep_init_*_resolution` |

統計を別スレッドから読むと 0 が返る。タイマー分解能はシステム全体に影響するので、`init` したら必ず `shutdown` すること。

`usleep_set_profile()` は **`spin_last_us` と `yield_policy` を上書きする**。`set_spin_last_us()` → `set_profile()` の順で呼ぶと設定が消える。

## DllMain の制約

`DllMain` はローダーロックを保持した状態で走る。ここから呼ばれる `shutdown_timer_resolution_impl()` / `shutdown_nt_resolution_impl()` の経路では:

- 関数ローカル `static` の**初回初期化を発生させない**（C++11 magic static は CRT の once ロックを取り、デッドロックしうる）
- ヒープ確保、他 DLL のロード、同期プリミティブでの待機をしない

`DLL_THREAD_DETACH` では `t_timer` をクローズする。`DLL_PROCESS_DETACH` では加えてシステム全体のタイマー分解能を戻す（OS が回収してくれないため必須）。

## ビルド

```bash
# Meson（推奨）— MSVC は x64 Native Tools プロンプトから
meson setup build --buildtype=release && meson compile -C build && meson test -C build

# MinGW / MSYS2
mingw32-make && mingw32-make test

# WinError 5 で sanity 実行が拒否される環境
powershell -ExecutionPolicy Bypass -File .\tools\meson_build_msvc.ps1 -RunTests
```

MSVC・MinGW の**両方**が通ることを変更のたびに確認する。片方だけで済ませない。

## テストの落とし穴

タイミングテストは「通っているのに何も検証していない」状態になりやすい。

- **上限だけのアサート（`dt < 10000`）は、実装が即 return してもパスする。** 下限も必ず置く。
- あるテストが特定の待機経路を検証したいなら、`usleep_stats_t` のカウンタ差分を直接アサートして**その経路を踏んだことを証明する**。設定値と入力値だけでは、実装の分岐を踏めているとは限らない。
- CI や仮想環境ではジッタが跳ねる。頑健性は中央値・p95 などの統計量で担保し、単発の最大値でフェイルさせない。ただし **CI を通すために本番の精度要件を下げない**。

## バージョン

`meson.build` の `version:` と `resource/usleep_win.rc` の FILEVERSION / PRODUCTVERSION / VALUE 文字列は**必ず一致させる**。

## ドキュメント

同じ事実が 4 箇所に散っている。API・既定値・閾値・バージョンを変えたら全部を更新すること。

- `README.md` — 日本語 README
- `document/README_en.md` — 英語 README（日本語版と内容が対応していること）
- `document/specsheet.md` — 実装仕様書。閾値・内部設計を記載
- `document/test_result.md` — 測定結果。**再測定していない数値を新しい結果として書かない**

性能を書くときは測定条件（CPU、電源プラン、タイマー分解能設定、負荷）を必ず併記する。Windows はハードリアルタイム OS ではないという前提の注意書きは維持する。

## コードスタイル

- インデントは**タブ**。コメントは日本語。見出しコメントは `// ---- 見出し ----` 形式
- `Makefile` のレシピ行はハードタブ必須（スペースだと `missing separator` で即死する）
- Win32 の新しめの API は `GetProcAddress` で動的解決し、不在時のフォールバックを用意する（古い Windows で DLL がロードすらできなくなるのを避けるため）

## サブエージェント

`.claude/agents/` に領域別のレビュー・修正エージェントを定義してある。作業内容に応じて使い分ける。

| エージェント | 担当領域 |
|---|---|
| `win-timing-auditor` | 待機経路・整数演算・TLS・DllMain ライフサイクル |
| `dll-abi-guardian` | 公開ヘッダの C ABI 互換性・呼び出し規約・バージョン整合 |
| `build-matrix-doctor` | Meson / Makefile / MSVC / CI |
| `timing-test-designer` | タイミングテストとベンチマークの設計・空振り検出 |
| `docs-sync-keeper` | README(日/英)・specsheet と実装の突き合わせ |
