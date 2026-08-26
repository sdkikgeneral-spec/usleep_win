# Makefile (MinGW / MSYS2)
# 使い方:
#   mingw32-make            # DLL / 静的ライブラリ / テスト / .pc をビルド
#   mingw32-make test       # テスト実行（共有版・静的版の順に実行）
#   mingw32-make install PREFIX=/mingw64
#   mingw32-make clean

CXX ?= g++
WINDRES ?= windres
AR ?= ar
# 警告レベルは meson.build の warning_level=3（MSVC の /W4 相当）と揃える。
# ソースは UTF-8 だが g++ は入力・実行文字セットとも既定が UTF-8 のため
# -finput-charset/-fexec-charset の明示は不要（MSVC 側のみ /utf-8 が要る）。
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?=
LIBS := -lwinmm

# ---- インストール先 ----
# meson の既定レイアウト（bin/ include/ lib/ lib/pkgconfig/）に合わせる。
PREFIX ?= /usr/local
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig

# バージョンは include/usleep_win.h を唯一の情報源として取り出す。
# ここに数値を直書きすると meson.build / .rc / ヘッダに加えて 4 重管理になる。
VER_MAJOR := $(shell sed -n 's/^#define USLEEP_WIN_VERSION_MAJOR[[:space:]]*\([0-9][0-9]*\).*/\1/p' include/usleep_win.h)
VER_MINOR := $(shell sed -n 's/^#define USLEEP_WIN_VERSION_MINOR[[:space:]]*\([0-9][0-9]*\).*/\1/p' include/usleep_win.h)
VER_PATCH := $(shell sed -n 's/^#define USLEEP_WIN_VERSION_PATCH[[:space:]]*\([0-9][0-9]*\).*/\1/p' include/usleep_win.h)
VERSION := $(VER_MAJOR).$(VER_MINOR).$(VER_PATCH)

BUILD := build
DLL := $(BUILD)/usleep_win.dll
IMPLIB := $(BUILD)/libusleep_win.a
TEST_EXE := $(BUILD)/test_usleep.exe

# ---- 静的ライブラリ ----
# 名前は meson の static_library('usleep_win_static') と揃える。
# インポートライブラリ libusleep_win.a と同名にはできない点に注意。
#
# 静的リンクでは DllMain が呼ばれない。スレッド終了時の WaitableTimer
# クローズと、プロセス終了時のタイマー分解能の復帰が自動で走らないので、
# 利用者が usleep_shutdown_timer_resolution() /
# usleep_shutdown_nt_resolution() を明示的に呼ぶ必要がある。
STATIC_LIB := $(BUILD)/libusleep_win_static.a
STATIC_OBJ := $(BUILD)/usleep_ex_static.o
TEST_STATIC_EXE := $(BUILD)/test_usleep_static.exe

PC_SHARED := $(BUILD)/usleep_win.pc
PC_STATIC := $(BUILD)/usleep_win-static.pc

SRC := src/usleep_ex.cpp
TEST_SRC := tests/test_usleep.cpp

BENCH_EXE := $(BUILD)/bench_usleep_csv.exe
BENCH_SRC := tools/bench_usleep_csv.cpp

# バージョンリソース。meson.build 側は windows.compile_resources() で必ず
# 埋め込むので、Makefile 側でも同じ成果物になるよう windres で埋め込む。
RC_SRC := resource/usleep_win.rc
RC_OBJ := $(BUILD)/usleep_win_rc.o

.PHONY: all run test clean install

all: $(DLL) $(STATIC_LIB) $(TEST_EXE) $(TEST_STATIC_EXE) $(BENCH_EXE) $(PC_SHARED) $(PC_STATIC)

$(BUILD):
	@mkdir -p $(BUILD)

$(RC_OBJ): $(RC_SRC) | $(BUILD)
	$(WINDRES) -I include -i $(RC_SRC) -o $(RC_OBJ)

$(DLL): $(SRC) include/usleep_win.h $(RC_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) -DUSLEEPWIN_EXPORTS -shared -o $(DLL) $(SRC) $(RC_OBJ) $(LIBS) -Wl,--out-implib,$(IMPLIB)

$(STATIC_OBJ): $(SRC) include/usleep_win.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -DUSLEEPWIN_STATIC -c -o $(STATIC_OBJ) $(SRC)

$(STATIC_LIB): $(STATIC_OBJ)
	$(AR) rcs $(STATIC_LIB) $(STATIC_OBJ)

$(TEST_EXE): $(TEST_SRC) include/usleep_win.h $(DLL)
	$(CXX) $(CXXFLAGS) -o $(TEST_EXE) $(TEST_SRC) -L$(BUILD) -lusleep_win $(LIBS)

# 静的リンク版テスト。USLEEPWIN_STATIC が実際にリンク・実行まで通ることを
# ビルドのたびに確認する（ヘッダのマクロ分岐だけでは実機で通る保証がない）。
$(TEST_STATIC_EXE): $(TEST_SRC) include/usleep_win.h $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) -DUSLEEPWIN_STATIC -o $(TEST_STATIC_EXE) $(TEST_SRC) -L$(BUILD) -lusleep_win_static $(LIBS)

$(BENCH_EXE): $(BENCH_SRC) include/usleep_win.h $(DLL)
	$(CXX) $(CXXFLAGS) -o $(BENCH_EXE) $(BENCH_SRC) -L$(BUILD) -lusleep_win $(LIBS)

# ---- pkg-config ----
# meson の pkgconfig.generate() が出すものと同じ内容を Makefile 側でも作る。
$(PC_SHARED): include/usleep_win.h | $(BUILD)
	@printf '%s\n' \
		'prefix=$(PREFIX)' \
		'includedir=$${prefix}/include' \
		'libdir=$${prefix}/lib' \
		'' \
		'Name: usleep_win' \
		'Description: Practical microsecond sleep for Windows (shared library)' \
		'Version: $(VERSION)' \
		'Libs: -L$${libdir} -lusleep_win' \
		'Libs.private: -lwinmm' \
		'Cflags: -I$${includedir}' > $(PC_SHARED)

$(PC_STATIC): include/usleep_win.h | $(BUILD)
	@printf '%s\n' \
		'prefix=$(PREFIX)' \
		'includedir=$${prefix}/include' \
		'libdir=$${prefix}/lib' \
		'' \
		'Name: usleep_win-static' \
		'Description: Practical microsecond sleep for Windows (static library)' \
		'Version: $(VERSION)' \
		'Libs: -L$${libdir} -lusleep_win_static -lwinmm' \
		'Cflags: -I$${includedir} -DUSLEEPWIN_STATIC' > $(PC_STATIC)

run: test

# 共有版と静的版を順番に実行する。どちらも実時間の待機を測るテストなので
# 並列に走らせてはいけない（タイマー分解能設定と CPU 競合で結果が汚れる）。
test: all
	@echo "Running tests (shared)..." && \
	PATH=$(BUILD):$$PATH $(TEST_EXE)
	@echo "Running tests (static)..."
	$(TEST_STATIC_EXE)

# ---- インストール ----
# meson install と同じレイアウトに置く。
install: all
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBDIR)" "$(DESTDIR)$(INCLUDEDIR)" "$(DESTDIR)$(PKGCONFIGDIR)"
	install -m 755 $(DLL) "$(DESTDIR)$(BINDIR)/"
	install -m 644 $(IMPLIB) "$(DESTDIR)$(LIBDIR)/"
	install -m 644 $(STATIC_LIB) "$(DESTDIR)$(LIBDIR)/"
	install -m 644 include/usleep_win.h "$(DESTDIR)$(INCLUDEDIR)/"
	install -m 644 $(PC_SHARED) "$(DESTDIR)$(PKGCONFIGDIR)/"
	install -m 644 $(PC_STATIC) "$(DESTDIR)$(PKGCONFIGDIR)/"

clean:
	rm -rf $(BUILD)
