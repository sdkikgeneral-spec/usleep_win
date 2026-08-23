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

# warning_level=3 は MSVC で /W4、GCC で -Wall -Wextra -Wpedantic に対応する。
# Makefile（MinGW 用）の CXXFLAGS と同じ警告レベルに揃えてある。
project('usleep_win', 'cpp',
  version: '0.2.1',
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

uslp = shared_library(
  'usleep_win',
  sources: uslp_sources,
  include_directories: inc,
  cpp_args: cflags + ['-DUSLEEPWIN_EXPORTS'],
  link_args: ldflags,
  dependencies: [winmm],
  install: false,
)

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
test('usleep-basic', exe_test, timeout: 180)

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
