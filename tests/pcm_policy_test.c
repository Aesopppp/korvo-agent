#include "pcm_policy.h"
#define CHECK(c) do { if (!(c)) return __LINE__; } while(0)
int main(void)
{
    CHECK(!pcm_can_start(2399, false));
    CHECK(pcm_can_start(2400, false));
    CHECK(!pcm_can_start(0, true));
    CHECK(pcm_can_start(1, true));
    CHECK(pcm_take_count(42) == 42);
    CHECK(pcm_take_count(8000) == 160);
    /* A short answer at EOF drains in order, including an incomplete block. */
    size_t left = 321, played = 0, blocks = 0;
    while(left) { size_t n=pcm_take_count(left); left-=n; played+=n; ++blocks; }
    CHECK(played == 321 && blocks == 3);
    CHECK(PCM_PREFILL < PCM_CAPACITY);
    CHECK(pcm_reference_available(0) == 0);
    CHECK(pcm_reference_available(PCM_DMA_SAMPLES) == 0);
    CHECK(pcm_reference_available(PCM_DMA_SAMPLES + 1024) == 1024);
    CHECK(pcm_reference_skip(PCM_DMA_SAMPLES + 1024, 1024) == 0);
    CHECK(pcm_reference_skip(PCM_DMA_SAMPLES + 1024 + 160, 1024) == 160);
    CHECK(pcm_reference_skip(PCM_DMA_SAMPLES - 1, 1024) == 0);
    CHECK(pcm_scale(-32768, 1, 2) == -16384);
    CHECK(pcm_scale(32767, 1, 2) == 16383);
    CHECK(pcm_scale(-32768, 0, 160) == 0);
    CHECK(pcm_scale(-32768, 160, 160) == -32768);
    for(unsigned i=0; i<160; ++i) {
        CHECK(pcm_scale(-32000,160-i-1,160) <= 0);
        CHECK(pcm_scale(32000,160-i-1,160) >= 0);
    }
    return 0;
}
