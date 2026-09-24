#include "reply_boundary.h"
#include "response_flow.h"
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void)
{
    CHECK(!reply_boundary_received(0, 0));
    CHECK(!reply_boundary_received(2, 1));
    CHECK(!reply_boundary_received(2, 0));
    CHECK(reply_boundary_received(2, 2));
    CHECK(!reply_packet_current(0, 0));
    CHECK(!reply_packet_current(3, 2));
    CHECK(reply_packet_current(3, 3));

    response_phase_t phase = RESPONSE_IDLE;
    response_expect(&phase);
    CHECK(response_accept_audio(&phase));
    /* Server finished already; barge-in discards the queued local tail. */
    CHECK(response_begin_cancel(&phase));
    CHECK(reply_boundary_received(1, 1));
    CHECK(response_finish(&phase) == RESPONSE_END_CANCELLED);
    CHECK(!response_accept_audio(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_IGNORE);
    response_expect(&phase);
    /* Old queue entries cannot end/play a new response. */
    CHECK(!reply_packet_current(2, 1));
    CHECK(phase == RESPONSE_WAITING);
    CHECK(reply_packet_current(2, 2));
    CHECK(response_accept_audio(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_NORMAL);

    /* Live server: no synthetic success before a matching boundary. */
    response_expect(&phase);
    CHECK(response_begin_cancel(&phase));
    CHECK(!reply_boundary_received(3, 2));
    CHECK(!response_accept_audio(&phase));
    CHECK(reply_boundary_received(3, 3));
    CHECK(response_finish(&phase) == RESPONSE_END_CANCELLED);
    return 0;
}
