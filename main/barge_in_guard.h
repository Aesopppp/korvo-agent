#ifndef BARGE_IN_GUARD_H
#define BARGE_IN_GUARD_H

#include <stdbool.h>
#include <stdint.h>

/* Conservative playback-only gate; does not change WakeNet or listening VAD.
 * These are tuning defaults, not evidence that the AEC reference is aligned. */
#define BARGE_IN_GUARD_MS 1200u
#define BARGE_IN_MIN_ENERGY 12000
#define BARGE_IN_CONFIRM_SAMPLES 7680u /* 480 ms at 16 kHz */
#define BARGE_IN_DIAGNOSTIC_ENERGY 2500

typedef struct {
    uint32_t samples;
    int32_t min_energy;
    int32_t max_energy;
} barge_in_guard_t;

static inline void barge_in_guard_reset(barge_in_guard_t *guard)
{
    guard->samples = 0;
    guard->min_energy = 0;
    guard->max_energy = 0;
}

static inline bool barge_in_guard_feed(barge_in_guard_t *guard, bool speech,
                                      int32_t energy, uint32_t samples,
                                      uint32_t playback_ms)
{
    if (playback_ms < BARGE_IN_GUARD_MS || !speech || samples == 0 ||
        energy < BARGE_IN_MIN_ENERGY) {
        barge_in_guard_reset(guard);
        return false;
    }
    if (!guard->samples || energy < guard->min_energy) guard->min_energy = energy;
    if (energy > guard->max_energy) guard->max_energy = energy;
    uint32_t remaining = BARGE_IN_CONFIRM_SAMPLES - guard->samples;
    guard->samples += samples < remaining ? samples : remaining;
    return guard->samples == BARGE_IN_CONFIRM_SAMPLES;
}
#endif
