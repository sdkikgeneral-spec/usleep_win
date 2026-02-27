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

---

## 📁 Directory Layout
```
include/
 └ usleep_win.h
src/
 └ usleep_ex.cpp
tools/
 └ bench_usleep_csv.cpp
tests/
 └ test_usleep.cpp
```

---

## 📜 License
MIT License (feel free to change as needed)

---

## 🤝 Contributing
Issues and PRs are welcome—tuning ideas, improvements, and measurement reports are highly appreciated.
