// SPDX-License-Identifier: MIT

//============================================================
// 簡易テスト
//   - 目標：基本的なタイミング、締切待機のドリフト、Sleep(0)による譲りをざっくり検証
//   - 注意：CI/仮想環境/省電力設定ではジッタが大きくなりやすいので閾値は緩め
//============================================================

#include <cstdio>
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include "../include/usleep_win.h"

static uint64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void assert_true(bool cond, const char* msg)
{
    if (!cond)
    {
        std::fprintf(stderr, "[FAIL] %s\n", msg);
        std::exit(1);
    }
}

int main()
{
    std::puts("[TEST] usleep_win basic timing...");

    // サーバー向け既定：BALANCED + Sleep(0) + 200us tail spin（Linux usleep(1)の感覚に寄せる）
    usleep_set_profile(USLP_BALANCED);
    usleep_set_yield_policy(USLP_YIELD_SLEEP0);
    usleep_set_spin_last_us(200);

    {
        uint64_t t0 = now_us();
        usleep_win(200); // 0.2ms
        uint64_t dt = now_us() - t0;
        std::printf("  usleep_win(200us) -> %llu us\n", (unsigned long long)dt);
        // 一般的な環境で10msを大きく超えない程度
        assert_true(dt < 10000, "200us should not overshoot >10ms in normal env");
    }

    {
        uint64_t t0 = now_us();
        usleep_win(2000); // 2ms
        uint64_t dt = now_us() - t0;
        std::printf("  usleep_win(2ms)   -> %llu us\n", (unsigned long long)dt);
        assert_true(dt > 500,  "2ms should not be near-zero");
        assert_true(dt < 50000, "2ms should not overshoot >50ms in normal env");
    }

    {
        uint64_t t0 = now_us();
        usleep_win(20000); // 20ms
        uint64_t dt = now_us() - t0;
        std::printf("  usleep_win(20ms)  -> %llu us\n", (unsigned long long)dt);
        assert_true(dt > 5000,  "20ms should be at least a few ms");
        assert_true(dt < 200000, "20ms should not overshoot >200ms in normal env");
    }

    std::puts("[TEST] deadline scheduling (drift check)...");
    {
        const uint64_t tick = 1000; // 1ms
        uint64_t next = now_us();
        const int N = 20;
        uint64_t max_late = 0;
        for (int i=0;i<N;i++)
        {
            next += tick;
            usleep_until_steady_us(next);
            uint64_t late = (now_us() > next) ? (now_us()-next) : 0;
            if (late > max_late) max_late = late;
        }
        std::printf("  max late: %llu us\n", (unsigned long long)max_late);
        // 典型環境では数ms〜十数ms以内に収まる想定
        assert_true(max_late < 10000, "deadline drift too large (>10ms)");
    }

    std::puts("[TEST] yield behavior with Sleep(0)...");
    {
        std::atomic<bool> stop{false};
        std::atomic<int>  ticks{0};
        std::thread worker([&]
        {
            // 同優先度のワーカー：Sleep(0)が有効ならメインが“譲って”進む
            while (!stop.load(std::memory_order_relaxed))
            {
                ticks.fetch_add(1, std::memory_order_relaxed);
                // 擬似的な軽作業
            }
        });

        // メイン側：超短い待機を多数回→ Sleep(0)により自発的に譲られる期待
        const int loops = 20000;
        for (int i=0;i<loops;i++)
        {
            usleep_win(1); // Linux usleep(1) 的な“薄い休止”
        }
        stop.store(true, std::memory_order_relaxed);
        worker.join();
        std::printf("  worker ticks = %d\n", ticks.load());
        assert_true(ticks.load() > 100, "worker should have run (Sleep(0) yielded)");
    }

    std::puts("[OK] all tests passed.");
    return 0;
}
