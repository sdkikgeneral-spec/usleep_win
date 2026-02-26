//============================================================
// ベンチ: ジッタ/CPU使用率/譲り回数をCSV出力
//   使い方(例): bench_usleep_csv.exe [iters] [tick_us] [spin_last_us] [yield_policy]
//     iters         : 反復回数（既定 2000）
//     tick_us       : 目標周期[µs]（既定 1000 = 1ms）
//     spin_last_us  : 最後のスピン時間[µs]（既定 200）
//     yield_policy  : 0=NONE 1=SWITCH 2=SLEEP0 3=SLEEP1（既定 2=SLEEP0）
//   出力列: iter,late_us,cpu_pct,spin_relax,yield_switch,yield_sleep0,yield_sleep1,timer_used
//============================================================
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <chrono>
#include "../include/usleep_win.h"

static inline uint64_t qpc_now_us()
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000ULL) / (uint64_t)f.QuadPart);
}

static inline uint64_t filetime_to_us(const FILETIME &ft)
{
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (uint64_t)(u.QuadPart / 10ULL); // 100ns -> us
}

int main(int argc, char** argv)
{
    int iters = (argc > 1) ? atoi(argv[1]) : 2000;
    uint64_t tick_us = (argc > 2) ? _strtoui64(argv[2], nullptr, 10) : 1000ULL;
    unsigned spin_last_us = (argc > 3) ? (unsigned)strtoul(argv[3], nullptr, 10) : 200U;
    int yield_policy = (argc > 4) ? atoi(argv[4]) : USLP_YIELD_SLEEP0;

    // 既定設定（BALANCED）を明示
    usleep_set_profile(USLP_BALANCED);
    usleep_set_yield_policy(yield_policy);
    usleep_set_spin_last_us(spin_last_us);

    // CSVヘッダ
    printf("iter,late_us,cpu_pct,spin_relax,yield_switch,yield_sleep0,yield_sleep1,timer_used\n");

    // 計測ループ
    uint64_t next = usleep_now_steady_us();

    // CPU使用率: 差分で近似
    FILETIME c0, e0, k0, u0, c1, e1, k1, u1;
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
    uint64_t wall0 = qpc_now_us();

    usleep_stats_t s_prev{}, s_now{};
    usleep_reset_stats();
    usleep_get_stats(&s_prev);

    for (int i=0; i<iters; ++i)
    {
        next += tick_us;
        usleep_until_steady_us(next);

        // 遅着（負は0丸め）
        uint64_t now = usleep_now_steady_us();
        uint64_t late = (now > next) ? (now - next) : 0ULL;

        // CPU%（このイテレーション区間）
        GetProcessTimes(GetCurrentProcess(), &c1, &e1, &k1, &u1);
        uint64_t wall1 = qpc_now_us();
        uint64_t cpu_us = (filetime_to_us(k1) + filetime_to_us(u1)) - (filetime_to_us(k0) + filetime_to_us(u0));
        uint64_t wall_us = wall1 - wall0;
        double cpu_pct = (wall_us > 0) ? (double)cpu_us / (double)wall_us * 100.0 : 0.0;
        // 次回差分用に更新
        k0=k1; u0=u1; wall0=wall1;

        // Stats 差分（スレッドローカル）
        usleep_get_stats(&s_now);
        uint64_t d_spin   = s_now.spin_relax    - s_prev.spin_relax;
        uint64_t d_swth   = s_now.yield_switch  - s_prev.yield_switch;
        uint64_t d_s0     = s_now.yield_sleep0  - s_prev.yield_sleep0;
        uint64_t d_s1     = s_now.yield_sleep1  - s_prev.yield_sleep1;
        uint64_t d_timer  = s_now.waitable_timer_uses - s_prev.waitable_timer_uses;
        s_prev = s_now;

        printf("%d,%llu,%.2f,%llu,%llu,%llu,%llu,%llu\n",
            i,
            (unsigned long long)late,
            cpu_pct,
            (unsigned long long)d_spin,
            (unsigned long long)d_swth,
            (unsigned long long)d_s0,
            (unsigned long long)d_s1,
            (unsigned long long)d_timer
        );
    }
    return 0;
}
