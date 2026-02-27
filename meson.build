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

project('usleep_win', 'cpp', version: '0.2.0', default_options: ['cpp_std=c++17'])

opt_native = get_option('native')
cflags = []
if host_machine.system() == 'windows'
  cc = meson.get_compiler('cpp')
  if cc.get_id() == 'msvc'
    if opt_native
      cflags += ['/O2', '/GL']
    else
      cflags += ['/O2']
    endif
  else
    if opt_native
      cflags += ['-O3', '-flto', '-march=native', '-mtune=native']
    else
      cflags += ['-O3', '-flto', '-march=x86-64-v2', '-mtune=generic']
    endif
  endif
endif

cc = meson.get_compiler('cpp')
winmm = cc.find_library('winmm', required: true)
inc = include_directories('include')

uslp_sources = ['src/usleep_ex.cpp']
if host_machine.system() == 'windows'
  uslp_sources += ['resource/usleep_win.rc']
endif

uslp = shared_library(
  'usleep_win',
  sources: uslp_sources,
  include_directories: inc,
  cpp_args: cflags + ['-DUSLEEPWIN_EXPORTS'],
  dependencies: [winmm],
  install: false,
)

# 単体テスト
exe_test = executable(
  'test_usleep',
  sources: ['tests/test_usleep.cpp'],
  include_directories: inc,
  link_with: [uslp],
  dependencies: [winmm],
)
test('usleep-basic', exe_test)

# CSVベンチ
exe_bench = executable(
  'bench_usleep_csv',
  sources: ['tools/bench_usleep_csv.cpp'],
  include_directories: inc,
  link_with: [uslp],
  dependencies: [winmm],
  install: false,
)