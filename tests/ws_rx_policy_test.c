/* Portable regression checks for the reported cancel race and RX burst. */
#include "ws_rx_policy.h"
#include "response_flow.h"
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void)
{
    const char message[] = "Conversation has none active response";
    const char code[] = "response_cancel_not_active";
    CHECK(ws_cancel_inactive(NULL, 0, message, sizeof(message) - 1));
    CHECK(ws_cancel_inactive(code, sizeof(code) - 1, NULL, 0));
    CHECK(!ws_cancel_inactive(NULL, 0, NULL, 0));
    CHECK(!ws_cancel_inactive(NULL, 0, message, sizeof(message) - 2));
    CHECK(!ws_cancel_inactive(NULL, 0, "Invalid token", 13));
    /* Borrowed JSON fields are not NUL terminated; honor their lengths. */
    const char embedded[] = "Conversation has none active response\",\"other\":1}";
    CHECK(ws_cancel_inactive(NULL, 0, embedded, sizeof(message) - 1));
    CHECK(!ws_cancel_inactive(NULL, 0, embedded, sizeof(embedded) - 1));

    response_phase_t phase = RESPONSE_IDLE;
    response_expect(&phase);
    CHECK(response_accept_audio(&phase));
    CHECK(response_begin_cancel(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_CANCELLED);
    /* Log sequence: done, listening, then the delayed no-active error.
     * It must not emit another completion or discard a new response. */
    CHECK(ws_cancel_inactive(NULL, 0, message, sizeof(message) - 1));
    CHECK(response_finish(&phase) == RESPONSE_END_IGNORE);
    response_expect(&phase);
    CHECK(ws_cancel_inactive(NULL, 0, message, sizeof(message) - 1));
    CHECK(phase == RESPONSE_WAITING);
    CHECK(response_accept_audio(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_NORMAL);

    /* A 30-second burst of 60-ms, ~180-byte packets overflowed the old 96
     * slots. It must now fit without dropping sound or control boundaries. */
    for (size_t i = 0; i < 500; ++i)
        CHECK(ws_rx_audio_fits(i, i * 181, 181));
    CHECK(ws_rx_audio_fits(WS_RX_FRAMES - WS_RX_CONTROL_RESERVE - 1, 0, 1));
    CHECK(!ws_rx_audio_fits(WS_RX_FRAMES - WS_RX_CONTROL_RESERVE, 0, 1));
    CHECK(ws_rx_audio_fits(0, WS_RX_AUDIO_BYTES - 181, 181));
    CHECK(!ws_rx_audio_fits(0, WS_RX_AUDIO_BYTES - 180, 181));
    CHECK(!ws_rx_audio_fits(0, WS_RX_AUDIO_BYTES + 1, 1));
    return 0;
}
