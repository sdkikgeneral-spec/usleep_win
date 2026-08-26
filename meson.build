# meson.build (root)
# 使い方:
#   # MSVC の場合（x64 Native Tools コマンドプロンプト）
#   meson setup build-msvc --buildtype=release
#   meson compile -C build-msvc
#   meson test -C build-msvc
#
#   # WinError 5 で sanity 実行が拒否される環境
#   powershell -ExecutionPolicy Bypass -File .\tools\meson_build_msvc.ps1 -RunTests
#
#   # MSYS2/MinGW の場合（MINGW64 シェル）
#   meson setup build-mingw --buildtype=release
#   meson compile -C build-mingw
#   meson test -C build-mingw
#
#   # 配布物（DLL / インポートライブラリ / 静的ライブラリ / ヘッダ / .pc）
#   meson install -C build --destdir stage
#
#   # CI と同じく警告をエラー扱いにする
#   meson setup build --buildtype=release -Dwerror=true

# warning_level=3 は MSVC で /W4、GCC で -Wall -Wextra -Wpedantic に対応する。
# Makefile（MinGW 用）の CXXFLAGS と同じ警告レベルに揃えてある。
project('usleep_win', 'cpp',
  version: '0.2.2',
  default_options: ['cpp_std=c++17', 'warning_level=3'],
)

cc = meson.get_compiler('cpp')
opt_native = get_option('native')

cflags = []
ldflags = []

if cc.get_id() == 'msvc'
  # ---- ソース文字コード ----
  # ソースは UTF-8（日本語コメントを含む）。/utf-8 が無いと MSVC は実行環境の
  # ANSI コードページ（日本語環境では CP932）として読み、UTF-8 のバイト列が
  # CP932 の 2 バイト文字と解釈されて行末の改行やコメント終端を食い潰す。
  # BOM の有無やコメントのバイト長に依存する不安定な状態になるため必ず渡す。
  cflags += ['/utf-8']

  cflags += ['/O2']
  if opt_native
    # /GL（全プログラム最適化）はリンク時 /LTCG とセットで指定する。
    # 片方だけだとリンカが警告を出すか、最適化が効かない。
    cflags += ['/GL']
    ldflags += ['/LTCG']
  endif
else
  # GCC/Clang は入力・実行文字セットとも既定が UTF-8 なので
  # -finput-charset/-fexec-charset の明示は不要。
  cflags += ['-O3', '-flto']
  ldflags += ['-flto']
  if opt_native
    cflags += ['-march=native', '-mtune=native']
  elif host_machine.cpu_family() == 'x86_64'
    # -march=x86-64-v2 は x86_64 専用。ARM64 に渡すと不正なフラグになる。
    cflags += ['-march=x86-64-v2', '-mtune=generic']
  endif
endif

winmm = cc.find_library('winmm', required: true)
inc = include_directories('include')

uslp_sources = ['src/usleep_ex.cpp']
if host_machine.system() == 'windows'
  windows_mod = import('windows')
  uslp_sources += windows_mod.compile_resources('resource/usleep_win.rc')
endif

# ---- 共有ライブラリ（既定の配布形態） ----
# install: true にすると Windows では DLL が bindir、インポートライブラリが
# libdir に入る。both_libraries() は使えない: 静的側は USLEEPWIN_EXPORTS ではなく
# USLEEPWIN_STATIC を定義する必要があり、cpp_args を共有できないため。
uslp = shared_library(
  'usleep_win',
  # name_prefix: '' で MinGW でも libusleep_win.dll ではなく usleep_win.dll に
  # なる。MSVC(meson) と Makefile(MinGW) の成果物名がこれで 3 系統とも揃う。
  name_prefix: '',
  sources: uslp_sources,
  include_directories: inc,
  cpp_args: cflags + ['-DUSLEEPWIN_EXPORTS'],
  link_args: ldflags,
  dependencies: [winmm],
  install: true,
)

# ---- 静的ライブラリ ----
# ターゲット名を分けているのは MSVC 対策。shared_library('usleep_win') が
# インポートライブラリとして usleep_win.lib を作るので、静的側を同名にすると
# 同じファイル名で衝突する。
#
# 注意: 静的リンクでは DllMain が呼ばれない。スレッド終了時の WaitableTimer
# クローズと、プロセス終了時のタイマー分解能の復帰が自動では走らないため、
# 利用者が usleep_shutdown_timer_resolution() / usleep_shutdown_nt_resolution()
# を明示的に呼ぶ必要がある。
#
# バージョンリソース(.rc)は静的ライブラリには含めない。FILEVERSION は
# モジュール（DLL/EXE）単位の情報であり、.lib に埋めても意味がないため。
#
# 注意（-Dnative=true のとき）: /GL 付きのオブジェクトを含む .lib を利用者が
# /LTCG 無しでリンクすると LNK4075 相当の警告になる。既定（native=false）では
# /GL を付けないのでこの問題は起きない。
uslp_static = static_library(
  'usleep_win_static',
  sources: ['src/usleep_ex.cpp'],
  include_directories: inc,
  cpp_args: cflags + ['-DUSLEEPWIN_STATIC'],
  dependencies: [winmm],
  install: true,
)

# ---- 公開ヘッダ / pkg-config ----
install_headers('include/usleep_win.h')

pkg = import('pkgconfig')

pkg.generate(
  uslp,
  name: 'usleep_win',
  filebase: 'usleep_win',
  description: 'Practical microsecond sleep for Windows (shared library)',
  version: meson.project_version(),
  libraries_private: ['-lwinmm'],
)

pkg.generate(
  uslp_static,
  name: 'usleep_win-static',
  filebase: 'usleep_win-static',
  description: 'Practical microsecond sleep for Windows (static library)',
  version: meson.project_version(),
  extra_cflags: ['-DUSLEEPWIN_STATIC'],
  libraries_private: ['-lwinmm'],
)

# ---- サブプロジェクトとして取り込まれたとき用 ----
usleep_win_dep = declare_dependency(
  link_with: uslp,
  include_directories: inc,
  dependencies: [winmm],
)

usleep_win_static_dep = declare_dependency(
  link_with: uslp_static,
  include_directories: inc,
  compile_args: ['-DUSLEEPWIN_STATIC'],
  dependencies: [winmm],
)

meson.override_dependency('usleep_win', usleep_win_dep)
meson.override_dependency('usleep_win-static', usleep_win_static_dep)

# 単体テスト
exe_test = executable(
  'test_usleep',
  sources: ['tests/test_usleep.cpp'],
  include_directories: inc,
  cpp_args: cflags,
  link_args: ldflags,
  link_with: [uslp],
  dependencies: [winmm],
)
# 実時間の待機を多数行うテストなので meson 既定の 30 秒では CI で不足しうる。
# ローカル(MSVC/release)の実測は約 1.9 秒。仮想化された CI ランナーの
# ジッタとスケジューリング遅延を見込んで十分な余裕を取る。
#
# is_parallel: false は必須。このテストは timeBeginPeriod /
# NtSetTimerResolution でシステム全体のタイマー分解能を触るうえ、
# 実時間の待機を測って下限・p95 を検証する。別のテストと同時に走ると
# 互いの分解能設定とCPU競合で結果が汚染される。
test('usleep-basic', exe_test, timeout: 180, is_parallel: false)

# 静的リンク版のテスト。USLEEPWIN_STATIC を定義して static_library に
# 直接リンクし、「静的ライブラリターゲットが実際にリンク・実行できる」ことを
# ビルドのたびに証明する（ヘッダのマクロ分岐だけでは実機で通る保証がない）。
exe_test_static = executable(
  'test_usleep_static',
  sources: ['tests/test_usleep.cpp'],
  include_directories: inc,
  cpp_args: cflags + ['-DUSLEEPWIN_STATIC'],
  link_args: ldflags,
  link_with: [uslp_static],
  dependencies: [winmm],
)
test('usleep-basic-static', exe_test_static, timeout: 180, is_parallel: false)

# CSVベンチ
exe_bench = executable(
  'bench_usleep_csv',
  sources: ['tools/bench_usleep_csv.cpp'],
  include_directories: inc,
  cpp_args: cflags,
  link_args: ldflags,
  link_with: [uslp],
  dependencies: [winmm],
  install: false,
)
