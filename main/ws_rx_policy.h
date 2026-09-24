#ifndef WS_RX_POLICY_H
#define WS_RX_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Up to about four minutes of 60 ms Opus frames, with a separate byte cap.
 * Control messages share FIFO ordering but audio may never consume all slots. */
#define WS_RX_FRAMES 4096u
#define WS_RX_CONTROL_RESERVE 32u
#define WS_RX_AUDIO_BYTES (1024u * 1024u)
#define WS_RX_CONTROL_BYTES (128u * 1024u)

static inline bool ws_rx_audio_fits(size_t queued, size_t bytes, size_t incoming)
{
    return queued < WS_RX_FRAMES - WS_RX_CONTROL_RESERVE &&
           bytes <= WS_RX_AUDIO_BYTES && incoming <= WS_RX_AUDIO_BYTES - bytes;
}

static inline bool ws_cancel_inactive(const char *code, size_t code_len,
                                     const char *message, size_t message_len)
{
    static const char expected_code[] = "response_cancel_not_active";
    static const char expected_message[] = "Conversation has none active response";
    return (code && code_len == sizeof(expected_code) - 1 &&
            memcmp(code, expected_code, code_len) == 0) ||
           (message && message_len == sizeof(expected_message) - 1 &&
            memcmp(message, expected_message, message_len) == 0);
}
#endif
