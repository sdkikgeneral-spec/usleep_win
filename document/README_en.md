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

> Note: Windows is not a hard real-time OS. Microsecond-scale waits are subject to power policies, virtualization, and system load.

---

## ✨ Features

### 🔧 High accuracy & low jitter
- **100 ns resolution** on systems with High-Resolution Waitable Timers
- Tightens timing by spinning only the **final 200–400 µs**

### 🧵 CPU-friendly waiting
- Emits `YieldProcessor()` during spin
- Inserts **periodic yield every 64 iterations** (`Sleep(0)` / `SwitchToThread()`) to improve fairness and reduce heat/noise
- Avoids pure busy-wait

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

---

## 📦 Build & Install

### Meson (recommended)
```bash
meson setup build --buildtype=release
meson compile -C build
```

### MinGW (Makefile)
```bash
mingw32-make
```

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

### 5) **Read per-thread stats (for benchmarking)**
```cpp
usleep_stats_t st{};
usleep_get_stats(&st);

printf("spin_relax=%llu\n", (unsigned long long)st.spin_relax);
printf("yield_sleep0=%llu\n", (unsigned long long)st.yield_sleep0);
```

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

### 1. BALANCED (default)
- Jitter: ★★★ / CPU: ★★☆ / Fairness: ★★★
- Behavior: cooperative `Sleep(0)` while far from deadline; **~250 µs tail spin**
- Use cases: 1 ms main loops, periodic I/O, server/game loops

### 2. STRICT (low jitter)
- Jitter: ★★★★ / CPU: ★★★★ / Fairness: ★★☆
- Behavior: thicker spin (300–500 µs), periodic `SwitchToThread()`
- Use cases: latency-sensitive, control loops, measurement/audio

### 3. LOW_POWER (power saving)
- Jitter: ★☆☆☆ / CPU: ★☆☆☆ / Fairness: ★★★★
- Behavior: mostly `Sleep(1)`, minimal or zero spin
- Use cases: background workers, battery-oriented systems

#### Tips for fine-tuning
- `usleep_set_spin_last_us(200–400)`: longer → lower jitter / higher CPU, shorter → lower CPU / higher jitter
- `usleep_set_yield_policy(...)`: `SLEEP0` (balanced), `SWITCH_THREAD` (locality), `SLEEP1` (power-saving)

---

## 📊 Benchmark (CSV)
Use `tools/bench_usleep_csv.cpp` to export per-iteration lateness/CPU%/yield counts as CSV.

```bash
bench_usleep_csv.exe 2000 1000 200 2 > result.csv
```

Columns:
```
iter,late_us,cpu_pct,spin_relax,yield_switch,yield_sleep0,yield_sleep1,timer_used
```

### Measured Comparison (2026-03-07)

The following results were measured on the same machine, running `2000 iter / 1000us tick` three times per configuration and averaging each metric.

- OS: Windows 10.0.26200.7922
- CPU: AMD Ryzen 7 5800H with Radeon Graphics
- Profile: fixed to `USLP_BALANCED`
- Data source: `bench_outputs/summary_runs.csv` / `bench_outputs/summary_agg.csv`

| config | setting | avg_late_us | p95_late_us | p99_late_us | max_late_us | avg_cpu_pct |
|---|---|---:|---:|---:|---:|---:|
| balanced_none_spin200 | `yield=NONE, spin=200` | 0.14 | 1.00 | 1.00 | 57.67 | 99.96 |
| balanced_sleep0_spin200 | `yield=SLEEP0, spin=200` | 0.25 | 0.67 | 1.00 | 115.33 | 99.46 |
| balanced_sleep0_spin300 | `yield=SLEEP0, spin=300` | 0.29 | 1.00 | 1.00 | 132.33 | 99.43 |
| balanced_switch_spin200 | `yield=SWITCH_THREAD, spin=200` | 0.31 | 0.33 | 1.00 | 203.33 | 98.90 |
| balanced_sleep1_spin200 | `yield=SLEEP1, spin=200` | 7505.87 | 14403.00 | 15150.00 | 15876.33 | 0.02 |

Notes:
- `SLEEP1` drastically reduces CPU usage, but latency becomes much larger for 1 ms periodic workloads.
- `NONE/SLEEP0/SWITCH_THREAD` keep latency low, but CPU usage remains high.
- In this benchmark, `timer_used=0`, which indicates convergence mainly by spin/yield behavior rather than waitable timer usage.

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
 └ bench_usleep_csv.cpp
tests/
 └ test_usleep.cpp
```

---

## 📜 License
This repository is licensed under the MIT License. See [LICENSE](../LICENSE) for details.

---

## 🤝 Contributing
Issues and PRs are welcome—tuning ideas, improvements, and measurement reports are highly appreciated.
