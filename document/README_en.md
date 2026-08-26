# usleep_win
High-accuracy, low-jitter `usleep()` for Windows (WaitableTimer + QPC + YieldProcessor)

`usleep_win` is a tiny, dependency-free C/C++ DLL/library delivering a **practical microsecond sleep with low jitter**, similar to Linux `usleep()`.

- High-Resolution Waitable Timer (100 ns)
- QueryPerformanceCounter (QPC)
- `YieldProcessor()` (x86: `PAUSE` / ARM64: `YIELD`)
- Hybrid approach: `Sleep(0)` / `SwitchToThread()` + short tail spin
- Server-friendly **BALANCED** preset (default)
- Built-in **stats API** for benchmarking
- **Deadline-based** `usleep_until_steady_us()` to minimize drift
- **C ABI with a pinned calling convention** (`USLEEP_CALL` = `__cdecl` on MSVC)
- **Version query API** to match the loaded DLL against the header you compiled with

> Note: Windows is not a hard real-time OS. Microsecond-scale waits are subject to power policies, virtualization, and system load.

---

## ✨ Features

### 🔧 High accuracy & low jitter
- **100 ns resolution** on systems with High-Resolution Waitable Timers
- Tightens timing by spinning only the **final 200–400 µs**

### 🧵 CPU-friendly waiting
- Emits `YieldProcessor()` during spin
- While still far from the deadline, inserts a **yield every 64 iterations** (`Sleep(0)` / `SwitchToThread()`) to improve fairness and reduce heat/noise
- Over the final `spin_last_us` it switches to a pure, non-yielding spin to tighten the landing
- Avoids pure busy-wait

> **`yield_policy` only applies to waits that take the spin path.** Once a wait goes through the
> WaitableTimer, the tail spin runs with a hard-coded `USLP_YIELD_NONE` and never looks at
> `yield_policy`. See "4) Tune profile / yield policy / tail spin" for the exact conditions.

### 🖥 Profiles
| Profile | Description |
|---|---|
| **BALANCED** (default) | Best for main loops / games / servers; low jitter and fairness |
| **STRICT** | Lower jitter; thicker spin and `SwitchToThread()` preference |
| **LOW_POWER** | Power-saving; mostly `Sleep(1)` (higher jitter) |

### ⚡ NT native API for sub-millisecond timer resolution (optional)
- Uses `NtSetTimerResolution` (ntdll.dll) to set system timer granularity as fine as **0.5 ms**
- More precise than `timeBeginPeriod(1)` which bottoms out at 1 ms
- `usleep_query_nt_resolution()` lets you check the minimum resolution supported on the current system before committing

### 📊 Stats API (per-thread)
- `spin_relax` (PAUSE/YIELD count)
- `yield_switch` / `yield_sleep0` / `yield_sleep1`
- `waitable_timer_uses`

### 🧩 State scope (important)

Settings and stats are **thread-local**; timer resolution is **process/system-wide**. Mixing the two up causes bugs.

| State | Scope | API |
|---|---|---|
| Profile / tail spin / yield policy / power mode | **Thread-local** | `usleep_set_profile` / `usleep_set_spin_last_us` / `usleep_set_yield_policy` / `usleep_set_power_mode` |
| Stats counters | **Thread-local** | `usleep_get_stats` / `usleep_reset_stats` |
| WaitableTimer handle | **Thread-local** | managed internally |
| System timer resolution | **Process / system-wide** | `usleep_init_timer_resolution` / `usleep_init_nt_resolution` |

- Configure every worker thread separately; one thread's settings never affect another.
- **Reading the stats from a different thread returns zeros.**
- Timer resolution is system-wide. Every `init_*` must be paired with the matching `shutdown_*` — the OS does not restore it for you.

### 🔖 Version query

Compare the header macros (compile time) with the DLL functions (run time).

```cpp
// Compile-time values
// USLEEP_WIN_VERSION_MAJOR / _MINOR / _PATCH
// USLEEP_WIN_VERSION_STRING  -> "0.2.2"
// USLEEP_WIN_VERSION_NUM     -> (major<<16) | (minor<<8) | patch

if (usleep_win_version() != USLEEP_WIN_VERSION_NUM) {
    printf("DLL is %s, header is %s\n",
           usleep_win_version_string(), USLEEP_WIN_VERSION_STRING);
}
```

- `usleep_win_version()` returns a packed `uint32_t` (`major<<16 | minor<<8 | patch`).
- `usleep_win_version_string()` returns a **static string literal** — never free it; it is thread-safe.
- Current version is **0.2.2** (consistent across `meson.build`, `usleep_win.rc`, and the header).

> **Changed in 0.2.2**:
> - `usleep_set_spin_last_us()` now enforces an upper bound, `USLEEP_SPIN_LAST_US_MAX`
>   (**10000 µs = 10 ms**). Values above it **return -1 and leave the setting untouched**
>   (it previously always returned 0). The default of 250 and the per-profile values
>   (0 / 250 / 400) are all within range, so normal usage is unaffected.
> - Fixed the `Sleep(ms)` fallback path taken when no high-resolution WaitableTimer is
>   available (pre-1803 Windows 10, WINE, ...): with `LOW_POWER` (`spin_last_us == 0`) it
>   returned without absorbing the µs-to-ms truncation (up to 999 µs short). The remainder
>   is now always spun out to the deadline, regardless of profile.
> - The static library is now a first-class build artifact, and `meson install` /
>   `mingw32-make install` lay out the DLL, import library, static library, header, and
>   pkg-config files.
> - Added GitHub Actions CI (MSVC / MinGW x Meson / Makefile, warnings treated as errors).

> **Fixed in 0.2.1**: `usleep_set_power_mode()` used a wrong internal constant for
> `ThreadPowerThrottling`, so it **always returned -1 for every mode and the power mode was never applied**.
> This is now fixed.

---

## 📦 Build & Install

### Meson (recommended)
```bash
meson setup build --buildtype=release
meson compile -C build
meson test -C build
```
With MSVC, run these from an **x64 Native Tools Command Prompt**.

### MinGW (Makefile)
```bash
mingw32-make
mingw32-make test
```

### When the MSVC sanity check is blocked with WinError 5
```powershell
powershell -ExecutionPolicy Bypass -File .\tools\meson_build_msvc.ps1 -RunTests
```

Both build systems produce the shared library, the static library, the tests (shared and
static builds) and the benchmark. `meson test -C build` / `mingw32-make test` run **both**
the shared-linked and the statically linked test binary.

### Installing (produce the distributables)

```bash
# Meson
meson install -C build --destdir stage

# Makefile (MinGW)
mingw32-make install PREFIX=/mingw64
```

Installed files:

| Path | Contents |
|---|---|
| `bin/usleep_win.dll` | Shared library (`name_prefix: ''` keeps the MinGW name `usleep_win.dll`, not `libusleep_win.dll`) |
| `lib/usleep_win.lib` | Import library (MSVC). MinGW + Meson produces `usleep_win.dll.a`; MinGW + Makefile produces `libusleep_win.a` |
| `lib/libusleep_win_static.a` | Static library (Meson emits this name on MSVC as well) |
| `include/usleep_win.h` | Public header |
| `lib/pkgconfig/usleep_win.pc` | pkg-config, shared |
| `lib/pkgconfig/usleep_win-static.pc` | pkg-config, static (its Cflags include `-DUSLEEPWIN_STATIC`) |

### CI

`.github/workflows/ci.yml` runs five jobs with **warnings treated as errors**
(`-Dwerror=true` for Meson, `-Werror` for the Makefile).

| Job | What it does |
|---|---|
| MSVC / Meson (`native=false` / `true`) | build -> test -> `meson install` |
| MinGW / Meson (`native=false` / `true`) | same, plus validating the generated `.pc` files with `pkgconf` |
| MinGW / Makefile | build -> test (shared + static) -> `install` -> cross-check artifact names against Meson |

The CI jobs run the test suite with **`USLEEP_TEST_CI=1`**, because scheduling delays spike on
shared, virtualized hosts. That mode relaxes **upper-bound assertions only**.

- Lower bounds (did the wait really last as long as requested — i.e. early-return detection) are
  never relaxed.
- Path proofs based on the stats counters (equality and lower bounds) are never relaxed either.
- Without the variable, the suite runs at the full production accuracy requirements.

### Linkage macros

| How you use it | Macro you define |
|---|---|
| Consume the DLL through its import library | none (defaults to `__declspec(dllimport)`) |
| Link against the static library (`libusleep_win_static.a`) / compile `src/usleep_ex.cpp` into your own target | **`USLEEPWIN_STATIC`** |
| Build the DLL itself | `USLEEPWIN_EXPORTS` (added automatically by Meson / the Makefile) |

Defining `USLEEPWIN_STATIC` drops `__declspec(dllimport)`. Forget it and the linker will
look for `__imp_`-prefixed symbols and fail to resolve them. If you consume the library
through pkg-config, `usleep_win-static.pc` already emits `-DUSLEEPWIN_STATIC`, so passing
`pkg-config --cflags usleep_win-static` verbatim cannot get it wrong.

> **⚠ `DllMain` does not run when you link statically.**
> With the DLL, `DLL_THREAD_DETACH` closes the per-thread `t_timer` handle and
> `DLL_PROCESS_DETACH` restores the system timer resolution. **Neither safety net exists in a
> static build.** If you call `usleep_init_timer_resolution()` / `usleep_init_nt_resolution()`,
> you must call `usleep_shutdown_timer_resolution()` / `usleep_shutdown_nt_resolution()`
> yourself before exiting — otherwise the system-wide timer resolution stays raised after
> your process is gone.

### Calling convention

Every public function is declared with `USLEEP_CALL` (`__cdecl` on MSVC), so the
convention is pinned regardless of your compiler switches. **Building your code with
`/Gz` (stdcall) or `/Gr` (fastcall) is safe** — a mismatch would corrupt the stack on x86.
On x64 / ARM64 there is only one convention, so the annotation is effectively a no-op.

> **Note**: `timeBeginPeriod(1)` affects the whole system. This library does not change it by default. Call `usleep_init_timer_resolution(1)` only when strictly needed and revert with `usleep_shutdown_timer_resolution()`.

---

## 📝 Usage Examples

### 1) **Simple microsecond sleep**
```cpp
#include "usleep_win.h"
#include <iostream>

int main() {
    usleep_win(300);   // sleep 300 µs
    std::cout << "Slept 300 µs\n";
    return 0;
}
```

### 2) **1 ms main loop (deadline-based, basic)**
```cpp
uint64_t next = usleep_now_steady_us();
const uint64_t tick = 1000; // 1 ms

for (;;) {
    next += tick;
    usleep_until_steady_us(next);  // deadline wait, reduces drift
    do_main_logic();
}
```

### 3) **1 ms main loop (deadline-based, advanced)**
```cpp
#include "usleep_win.h"
#include <iostream>

int main() {
    const uint64_t tick_us = 1000;  // 1 ms
    uint64_t next = usleep_now_steady_us();

    // Recommended stable settings
    usleep_set_profile(USLP_BALANCED);
    usleep_set_spin_last_us(250);          // tail spin = 250 µs
    usleep_set_yield_policy(USLP_YIELD_SLEEP0);

    for (int i = 0; i < 5000; i++) {
        next += tick_us;
        usleep_until_steady_us(next);

        uint64_t now = usleep_now_steady_us();
        int64_t late = (now > next) ? (now - next) : 0;
        std::cout << "tick " << i << " late=" << late << "us\n";

        // Recover from heavy overrun by re-syncing
        if (late > 5000) { // > 5 ms late
            next = now;
        }
    }
}
```

### 4) **Tune profile / yield policy / tail spin**
```cpp
// Lower jitter
usleep_set_profile(USLP_STRICT);

// Power-saving
usleep_set_profile(USLP_LOW_POWER);

// Yield policy
usleep_set_yield_policy(USLP_YIELD_SLEEP0);        // default: cooperative
usleep_set_yield_policy(USLP_YIELD_SWITCH_THREAD); // local to same CPU
usleep_set_yield_policy(USLP_YIELD_SLEEP1);        // stronger yield (higher jitter)

// Tail spin in microseconds
usleep_set_spin_last_us(250);  // recommended: 200–400
```

> **⚠ `yield_policy` only applies to waits that take the spin path.**
> When `do_sleep_us()` goes through the WaitableTimer, the tail spin is invoked as
> `spin_with_yield_until_us(target, 0, USLP_YIELD_NONE)`, so **`yield_policy` is never read**.
> Path selection depends only on **the requested wait length and high-resolution timer
> availability** — `spin_last_us` does not change which path is taken.

| Wait (with the BALANCED profile) | Path | `yield_policy` |
|---|---|---|
| `usec == 0` | `SwitchToThread()` | not used |
| `usec >= 2000` (`timer_first_us`) | WaitableTimer + pure spin | **not used** |
| HR timer available and `usec > 200` (`prefer_spin_below`) | WaitableTimer + pure spin | **not used** |
| `usec <= 200`, or a short wait on a system without an HR timer | Spin path | **used** |
| `LOW_POWER` on the spin path | Spin path | not used (hard-coded `Sleep(1)`) |

> The thresholds are per profile (`prefer_spin_below` is 500 for STRICT, 0 for LOW_POWER).
> On modern Windows (10 1803+), where the high-resolution WaitableTimer is available,
> **a 1 ms BALANCED wait always takes the timer path, so changing `yield_policy` changes nothing.**
> This is what the measurements show: across all 40 runs of the 1 ms / BALANCED / `spin_last_us=200`
> configuration (v0.2.2, 2026-08-26), `timer_used` covered essentially every iteration while
> `yield_switch` / `yield_sleep0` / `yield_sleep1` stayed at 0, and the spread between the four
> yield policies was run-to-run noise ([test_result.md](./test_result.md)).
> To make `yield_policy` matter, **keep the wait at or below `prefer_spin_below`** (≤ 200 µs for
> BALANCED), or target systems without an HR timer. Confirm it with `usleep_get_stats()`:
> look at `waitable_timer_uses` versus the `yield_*` counters.

> **Tail-spin upper bound**: `usleep_set_spin_last_us()` rejects anything above
> `USLEEP_SPIN_LAST_US_MAX` (**10000 µs = 10 ms**); it then **returns -1 and leaves the current
> setting unchanged** (0 on success). The tail spin pins one core at 100%, so allowing a spin
> longer than the default Windows timer granularity (~15.6 ms) or the default client quantum
> (~20-30 ms) would defeat the whole point — hiding timer coarseness behind a short spin —
> and would make accuracy worse, not better.

> **⚠ Call order matters**: `usleep_set_profile()` **overwrites both `spin_last_us` and
> `yield_policy`**. To keep custom values, always call **`set_profile()` first**, then
> `set_spin_last_us()` / `set_yield_policy()`. The reverse order silently discards your values.

```cpp
// ❌ discarded
usleep_set_spin_last_us(400);
usleep_set_profile(USLP_BALANCED);   // resets spin_last_us back to 250

// ✅ kept
usleep_set_profile(USLP_BALANCED);
usleep_set_spin_last_us(400);
```

### 5) **Read per-thread stats (for benchmarking)**
```cpp
usleep_stats_t st{};
usleep_get_stats(&st);

printf("spin_relax=%llu\n", (unsigned long long)st.spin_relax);
printf("yield_sleep0=%llu\n", (unsigned long long)st.yield_sleep0);
```

> The counters belong to the calling thread. **Reading them from another thread returns zeros.**

### 6) **Optional: enable 1 ms timer resolution**
```cpp
usleep_init_timer_resolution(1);   // system-wide effect
// ...
usleep_shutdown_timer_resolution();
```

### 7) **Optional: sub-millisecond timer resolution via NT native API**

`timeBeginPeriod(1)` is limited to 1 ms granularity. `usleep_init_nt_resolution` uses
`NtSetTimerResolution` (ntdll.dll) to reach **0.5 ms (5000 × 100 ns)** on most systems,
improving the accuracy of waitable-timer-based waits.

```cpp
// 1. Query what the system supports
unsigned min_res = 0, max_res = 0, cur_res = 0;
if (usleep_query_nt_resolution(&min_res, &max_res, &cur_res) == 0) {
    // min_res: coarsest value  (e.g. 156250 = 15.625 ms)
    // max_res: finest value    (e.g.   5000 =  0.5  ms)  ← use this
    // cur_res: current setting
}

// 2. Apply finest resolution (use max_res, or hard-code 5000)
usleep_init_nt_resolution(max_res);   // system-specific maximum precision
// or
usleep_init_nt_resolution(5000);      // fixed 0.5 ms (effective only if supported)

// 3. Use as normal — the underlying timer granularity is now finer
usleep_win(800);   // 800 µs sleep

// 4. Always restore when done (reverts system-wide change)
usleep_shutdown_nt_resolution();
```

> **Note**: Like `timeBeginPeriod`, this affects the entire system. Always call
> `usleep_shutdown_nt_resolution()` on exit or when precision is no longer needed.
> `NtSetTimerResolution` is undocumented but has been stable since Windows 2000.

| API | Granularity |
| --- | --- |
| `timeBeginPeriod(1)` | min **1.0 ms** |
| `usleep_init_nt_resolution(5000)` | min **0.5 ms** (where supported) |

---

## 🔧 Profile Tuning Guide

The per-profile threshold table, the internal design, and the detailed tuning notes live in the
**[spec sheet (specsheet.md)](./specsheet.md)**.

| Profile | Jitter | CPU | Fairness | Use cases |
|---|---|---|---|---|
| **BALANCED** (default) | ★★★ | ★★☆ | ★★★ | 1 ms main loops, game/server loops |
| **STRICT** | ★★★★ | ★★★★ | ★★☆ | Latency-sensitive work, audio |
| **LOW_POWER** | ★☆☆☆ | ★☆☆☆ | ★★★★ | Background workers, power saving |

### Measurement-based tuning notes

Everything below is an observation from **measurements on a single machine** (v0.2.2, 2026-08-26).
The source numbers and the full tables are in [test_result.md](./test_result.md).

Conditions: Intel Core Ultra 9 285K (24 logical cores) / Windows 11 Home 10.0.26200 / "Balanced"
power plan / MSVC 19.51 release build / deadline-based loop via `usleep_until_steady_us()`,
2000 iterations / BALANCED profile / **not a clean machine** — everyday apps were running and
system-wide CPU usage sat at 4–6%. CPU% is defined as **one logical core = 100%**
(from `QueryThreadCycleTime`, measuring thread only). All figures are ranges over repeated runs.

**What actually buys deadline accuracy is `spin_last_us`** (1 ms period, 1 ms timer resolution, n=3)

| `spin_last_us` | p50 lateness (µs) | p95 lateness (µs) | CPU% (1 core = 100) |
|---:|---:|---:|---:|
| 200 | 90–131 | 361–419 | 2.87–3.28 |
| 400 | 0 | 119–146 | 12.95–13.71 |
| 600 | 0 | 0 | 44.51–44.69 |

- **The accuracy/CPU trade-off lies on the `spin_last_us` axis.** You can drive p95 lateness to
  zero, but it costs roughly 45% of one logical core.
- Setting `spin_last_us` at or above the wait length (1000 µs at this period) leaves no room for
  the tail spin (internally `coarse_us` collapses to `usec`) and makes lateness worse.
  **Keep it well below the period.**

**Raising the timer resolution to 0.5 ms does not improve accuracy in this configuration**

- At a 1 ms period with `spin_last_us=200` (n=5), switching to 0.5 ms via
  `usleep_init_nt_resolution(5000)` moved p50 lateness from 71–130 µs to 132–149 µs — **worse**,
  not better — while CPU% dropped slightly, from 2.63–3.72% to 2.28–2.90%.
  **The mechanism was not identified in this measurement.** One relevant fact is that the library
  uses `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, which operates independently of the global timer
  resolution setting. Do not treat any causal story as established.
- Resolution buys an order of magnitude **only where `Sleep(1)` is on the path.** At a 200 µs
  period with `spin_last_us=0` and `USLP_YIELD_SLEEP1` (spin path), p50 lateness was
  7808–7906 µs at 1 ms resolution and 710–753 µs at 0.5 ms.
- Timer resolution is system-wide shared state, so the current default — **never touch it unless
  the application asks for it** — is the right one.

**A yield policy is not a way to lower CPU usage**

- At a 200 µs period with `spin_last_us=0` (i.e. genuinely on the spin path), `NONE`,
  `SWITCH_THREAD` and `SLEEP0` all hit the deadline almost perfectly (p50 and p95 lateness = 0 µs)
  but burn 99.34–100.44% CPU — **essentially one whole logical core**. `Sleep(0)` merely offers the
  CPU to other runnable threads; the calling thread keeps running.
- Only `SLEEP1` gives the CPU back (0.18–0.27% at 1 ms resolution, 0.71–0.82% at 0.5 ms), and in exchange the
  `Sleep(1)` dwell time becomes the lateness, so it cannot keep up with short periods
  (p50 ≈ 7.8 ms at 1 ms resolution, as above).

> All of this is **a snapshot of one machine in one configuration**. Windows is not a hard
> real-time OS, so another machine, power plan, or load level can change the trends themselves.
> Measure on your own target.

---

## 📊 Benchmark (CSV)
Use `tools/bench_usleep_csv.cpp` to export per-iteration lateness/CPU%/yield counts as CSV.

```bash
# args: [iters] [tick_us] [spin_last_us] [yield_policy] [profile] [label]
bench_usleep_csv.exe 2000 1000 200 2 0 balanced_sleep0_spin200 > result.csv
```

Columns (one row per iteration; every column except `late_us` and `cum_thr_cpu_pct` is a
per-iteration delta of `usleep_stats_t`):
```
iter,late_us,cum_thr_cpu_pct,spin_relax,yield_switch,yield_sleep0,yield_sleep1,timer_used
```

- `late_us` — arrival minus deadline (early arrival is clamped to 0).
- `cum_thr_cpu_pct` — **cumulative** CPU% since the start of the loop: the `QueryThreadCycleTime`
  cycle delta converted to time with a cycles/µs factor calibrated at startup, divided by the wall
  time (QPC) of the same span. **One logical core = 100%.** No per-iteration instantaneous value is
  emitted, because over a 1 ms span the quantization makes it meaningless.

A block of `#SUMMARY` lines follows, printed to **both stdout and stderr**, so you can redirect the
CSV and still collect the summaries from stderr:

| `#SUMMARY` field | Meaning |
|---|---|
| `label` / `profile` / `yield` / `spin_us` / `tick_us` / `iters` / `ncpu` | Measurement conditions |
| `avg_late_us` / `p50_late_us` / `p95_late_us` / `p99_late_us` / `max_late_us` | Lateness distribution |
| `thread_cycle_cpu_pct_of_1core` | **Primary metric**, from `QueryThreadCycleTime` |
| `thread_cpu_pct_of_1core` | Reference only, from `GetThreadTimes` (tick-sampled billing) |
| `process_cpu_pct_of_1core` / `process_cpu_pct_of_system` | All threads of the process |
| `cycles_per_us` / `thread_cycles` / `thread_cycle_cpu_us` | Calibration factor and raw cycle count |
| `wall_total_us` / `thread_cpu_us` / `cpu_us_per_iter` / `ns_per_spin_relax` | Derived sanity-check quantities |
| `nt_resolution_100ns min= max= cur= cur_end= query_ok= requested= acquired=` | Timer-resolution conditions, and whether another process stole it mid-run (**discard runs where `cur != cur_end`**) |
| `spin_relax` / `yield_switch` / `yield_sleep0` / `yield_sleep1` / `timer_used` | Which wait paths were actually taken |

Set `USLEEP_BENCH_NT_RES_100NS` to pin the timer resolution. Leaving it unset means the benchmark
**does not touch it at all** and measures the machine as other processes leave it. Use `max` for the
finest value the system reports, or a number in 100 ns units (`5000` = 0.5 ms). The benchmark always
calls `usleep_shutdown_nt_resolution()` afterwards.

> **Never read the numbers without checking the path.** Without the `timer_used` / `yield_*`
> breakdown you cannot tell whether the code path you meant to measure is the one that ran.

The CPU meter itself lives in `tools/cpu_accounting.h`, which `tests/test_usleep.cpp` includes as
well: `test_cpu_accounting()` uses the **same** header to assert that a pure busy wait reports
around 100% of one logical core and that a sleeping thread reports 0%. If the meter breaks, the
test fails before the benchmark can publish a wrong number.

Measured figures — with their measurement conditions, and the reasons the pre-v0.2.2 CPU numbers
were withdrawn — are in **[test results (test_result.md)](./test_result.md)**. Windows is not a hard
real-time OS: those figures are a snapshot of one machine under one configuration, not a guarantee.

---

## 📁 Directory Layout
```
include/
 └ usleep_win.h
src/
 └ usleep_ex.cpp
resource/
 └ usleep_win.rc
tools/
 ├ bench_usleep_csv.cpp   ... CSV benchmark
 ├ cpu_accounting.h       ... CPU usage meter (header shared by the benchmark and the tests)
 └ meson_build_msvc.ps1   ... MSVC build wrapper
tests/
 └ test_usleep.cpp
.github/workflows/
 └ ci.yml
```

`tools/cpu_accounting.h` is a pure header that is deliberately not registered with either build
system; both the benchmark and the tests include it, so the compiler guarantees that the CPU% the
benchmark prints and the CPU% the tests verify come from the same meter.

---

## 📜 License
This repository is licensed under the MIT License. See [LICENSE](../LICENSE) for details.

---

## 🤝 Contributing
Issues and PRs are welcome—tuning ideas, improvements, and measurement reports are highly appreciated.

### Source encoding
Save sources under `src/`, `include/`, `tests/`, and `tools/` as **UTF-8 with BOM**.

- Without a BOM, MSVC reads the file using the system ANSI code page (CP932 on Japanese systems).
- This matters most for the public header `include/usleep_win.h`: it **cannot control the
  consumer's compiler flags** (`/utf-8` may be absent), so a missing BOM produces C4819 on the
  consumer side and can swallow declarations that follow a Japanese comment.

### Reporting measurements
Always state the measurement conditions: CPU, OS build, power plan, timer-resolution settings,
and any concurrent load. Numbers without conditions cannot be compared.
