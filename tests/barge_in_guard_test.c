#include "barge_in_guard.h"
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void)
{
    barge_in_guard_t guard = {0};

    /* Logged false triggers must not cancel even a long answer. */
    for (unsigned i = 0; i < 2000; ++i) {
        CHECK(!barge_in_guard_feed(&guard, true, 2950, 512, 10000));
        CHECK(!barge_in_guard_feed(&guard, true, 3435, 512, 10000));
    }
    CHECK(guard.samples == 0);

    /* The initial playback settling interval never contributes speech time. */
    CHECK(!barge_in_guard_feed(&guard, true, 80000, 7680, 1199));
    CHECK(guard.samples == 0);
    for (unsigned i = 0; i < 14; ++i)
        CHECK(!barge_in_guard_feed(&guard, true, 12000, 512, 1200 + i * 32));
    CHECK(barge_in_guard_feed(&guard, true, 90000, 512, 1648));
    CHECK(guard.min_energy == 12000 && guard.max_energy == 90000);

    /* Duration is sample based: a different fetch size must not halve it. */
    barge_in_guard_reset(&guard);
    for (unsigned i = 0; i < 7; ++i)
        CHECK(!barge_in_guard_feed(&guard, true, 80000, 1024, 2000 + i * 64));
    CHECK(barge_in_guard_feed(&guard, true, 80000, 1024, 2448));

    /* Discontinuous loud bursts cannot accumulate into an interruption. */
    barge_in_guard_reset(&guard);
    for (unsigned burst = 0; burst < 10; ++burst) {
        for (unsigned i = 0; i < 14; ++i)
            CHECK(!barge_in_guard_feed(&guard, true, 80000, 512, 10000));
        CHECK(!barge_in_guard_feed(&guard, false, 80000, 512, 10000));
    }
    CHECK(!barge_in_guard_feed(&guard, true, 80000, 7168, 10000));
    CHECK(!barge_in_guard_feed(&guard, true, 11999, 512, 10000));
    CHECK(!barge_in_guard_feed(&guard, true, 80000, 512, 10000));
    CHECK(guard.samples == 512);

    /* Missing samples break continuity; a counter already at its limit
     * stays bounded if the caller retries a full event queue. */
    CHECK(!barge_in_guard_feed(&guard, true, 80000, 0, 10000));
    CHECK(guard.samples == 0);
    CHECK(barge_in_guard_feed(&guard, true, 80000, UINT32_MAX, 10000));
    CHECK(barge_in_guard_feed(&guard, true, 80000, 512, 10000));
    CHECK(guard.samples == 7680);
    barge_in_guard_reset(&guard);
    CHECK(guard.samples == 0 && guard.min_energy == 0 && guard.max_energy == 0);
    return 0;
}
