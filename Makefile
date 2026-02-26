# Makefile (MinGW / MSYS2)
# 使い方:
#   mingw32-make            # DLLとテストをビルド
#   mingw32-make test       # テスト実行（PATH に build/ を追加して実行）
#   mingw32-make clean

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude
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

all: $(DLL) $(TEST_EXE) $(BENCH_EXE)

$(BUILD):
    @mkdir -p $(BUILD)

$(DLL): $(BUILD) $(SRC) include/usleep_win.h
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
