#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PCM_CAPACITY 8000u
#define PCM_PREFILL 2400u
#define PCM_BLOCK 160u
#define PCM_DMA_BLOCKS 6u
static inline bool pcm_can_start(size_t count, bool eof)
{ return count >= PCM_PREFILL || (eof && count > 0); }
static inline size_t pcm_take_count(size_t count)
{ return count < PCM_BLOCK ? count : PCM_BLOCK; }
/* Submitted samples can still be in TX DMA: keep this horizon behind the
 * producer. Trim older history after a delayed microphone read. */
#define PCM_DMA_SAMPLES (PCM_DMA_BLOCKS * PCM_BLOCK)
static inline size_t pcm_reference_skip(size_t count, size_t requested)
{ return count > requested + PCM_DMA_SAMPLES ? count - requested - PCM_DMA_SAMPLES : 0; }
static inline size_t pcm_reference_available(size_t count)
{ return count > PCM_DMA_SAMPLES ? count - PCM_DMA_SAMPLES : 0; }
static inline int16_t pcm_scale(int16_t sample, unsigned gain, unsigned total)
{ return (int32_t)sample * (int32_t)gain / (int32_t)total; }
