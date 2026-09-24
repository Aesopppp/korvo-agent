#ifndef REPLY_BOUNDARY_H
#define REPLY_BOUNDARY_H

#include <stdbool.h>
#include <stdint.h>

/* A wire completion can arrive long before the speaker drains buffered Opus.
 * Zero denotes no response. Match an exact turn; never reuse old completion. */
static inline bool reply_boundary_received(uint32_t playing, uint32_t received_done)
{
    return playing != 0 && playing == received_done;
}

static inline bool reply_packet_current(uint32_t active, uint32_t packet)
{
    return active != 0 && active == packet;
}
#endif
