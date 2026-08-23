# Makefile (MinGW / MSYS2)
# 使い方:
#   mingw32-make            # DLLとテストをビルド
#   mingw32-make test       # テスト実行（PATH に build/ を追加して実行）
#   mingw32-make clean

CXX ?= g++
# 警告レベルは meson.build の warning_level=3（MSVC の /W4 相当）と揃える。
# ソースは UTF-8 だが g++ は入力・実行文字セットとも既定が UTF-8 のため
# -finput-charset/-fexec-charset の明示は不要（MSVC 側のみ /utf-8 が要る）。
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?=
LIBS := -lwinmm

BUILD := build
DLL := $(BUILD)/usleep_win.dll
IMPLIB := $(BUILD)/libusleep_win.a
TEST_EXE := $(BUILD)/test_usleep.exe

SRC := src/usleep_ex.cpp
TEST_SRC := tests/test_usleep.cpp

BENCH_EXE := $(BUILD)/bench_usleep_csv.exe
BENCH_SRC := tools/bench_usleep_csv.cpp

.PHONY: all run test clean

all: $(DLL) $(TEST_EXE) $(BENCH_EXE)

$(BUILD):
	@mkdir -p $(BUILD)

$(DLL): $(SRC) include/usleep_win.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -DUSLEEPWIN_EXPORTS -shared -o $(DLL) $(SRC) $(LIBS) -Wl,--out-implib,$(IMPLIB)

$(TEST_EXE): $(TEST_SRC) include/usleep_win.h $(DLL)
	$(CXX) $(CXXFLAGS) -o $(TEST_EXE) $(TEST_SRC) -L$(BUILD) -lusleep_win $(LIBS)

$(BENCH_EXE): $(BENCH_SRC) include/usleep_win.h $(DLL)
	$(CXX) $(CXXFLAGS) -o $(BENCH_EXE) $(BENCH_SRC) -L$(BUILD) -lusleep_win $(LIBS)

run: test

test: all
	@echo "Running tests..." && \
	PATH=$(BUILD):$$PATH $(TEST_EXE)

clean:
	rm -rf $(BUILD)
