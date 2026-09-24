/* Host regression tests for the same response transitions used by ws_service.
 * No ESP-IDF or host C runtime is required. A failed check returns its line. */
#include "response_flow.h"
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void)
{
    response_phase_t phase = RESPONSE_IDLE;
    CHECK(!response_accept_audio(&phase));
    CHECK(!response_begin_cancel(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_IGNORE);

    /* Normal multi-turn conversation. Completion is delivered exactly once. */
    for (int turn = 0; turn < 4; ++turn) {
        response_expect(&phase);
        CHECK(response_accept_audio(&phase));
        CHECK(response_accept_audio(&phase));
        CHECK(response_finish(&phase) == RESPONSE_END_NORMAL);
        CHECK(response_finish(&phase) == RESPONSE_END_IGNORE);
    }

    /* The logged failure: cancel while playing, late binary, old completion.
     * Old completion must not look like a second normal reply completion. */
    response_expect(&phase);
    CHECK(response_accept_audio(&phase));
    CHECK(response_begin_cancel(&phase));
    CHECK(!response_begin_cancel(&phase));
    CHECK(!response_accept_audio(&phase));
    response_expect(&phase); /* Premature new speech must not release cancel. */
    CHECK(phase == RESPONSE_CANCELLING);
    CHECK(response_finish(&phase) == RESPONSE_END_CANCELLED);
    CHECK(response_finish(&phase) == RESPONSE_END_IGNORE);
    CHECK(!response_accept_audio(&phase));

    /* A real subsequent user turn is allowed after the cancellation boundary. */
    response_expect(&phase);
    CHECK(response_accept_audio(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_NORMAL);

    /* Reply already completed when the delayed VAD event is handled. */
    CHECK(!response_begin_cancel(&phase));
    CHECK(!response_accept_audio(&phase));

    /* Audio worker wins the mutex while cancellation is only queued:
     * play every remaining frame, then ignore the delayed cancellation.
     * A scheduling hint must not bypass this phase-based admission. */
    response_expect(&phase);
    for (int frame = 0; frame < 20; ++frame)
        CHECK(response_accept_audio(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_NORMAL);
    CHECK(!response_begin_cancel(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_IGNORE);

    /* Cancellation wins the mutex: only subsequent frames are discarded,
     * and the boundary is cancellation, never a normal reply completion. */
    response_expect(&phase);
    CHECK(response_accept_audio(&phase));
    CHECK(response_begin_cancel(&phase));
    for (int frame = 0; frame < 20; ++frame)
        CHECK(!response_accept_audio(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_CANCELLED);

    /* Cancel before first audio frame; same acknowledgement path. */
    response_expect(&phase);
    CHECK(response_begin_cancel(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_CANCELLED);

    /* Recovery starts a clean session; old binary and done cannot awaken it. */
    phase = RESPONSE_IDLE;
    CHECK(!response_accept_audio(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_IGNORE);
    response_expect(&phase);
    CHECK(response_accept_audio(&phase));
    return 0;
}
