/**
 * proto_service.h -- ACOS V1 protocol frame parsing & construction.
 *
 *   - JSON parser: hand-written, no cJSON dependency.
 *   - Frame builders:
 *       auth                       (first message after WebSocket open)
 *       input_audio_buffer.append  (Opus, Base64 in JSON text frame)
 *
 *   Spec (ACOS API V1):
 *     - Upload: WebSocket text frame, Base64 Opus inside "audio" field,
 *               "encoding":"opus". Recommended 20 ms / 320 samples.
 *     - Download: WebSocket binary frame, raw Opus (60 ms / 960 samples).
 *
 *   Server -> Client JSON events:
 *     __proxy_connected__, input_audio_buffer.speech_started,
 *     input_audio_buffer.speech_stopped, response.audio.done, error.
 *
 *   Dependencies: <string.h>, <stdint.h>, esp_err.h.
 */

#ifndef PROTO_SERVICE_H
#define PROTO_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Parse result codes ------------------------------------------------- */
typedef enum {
    PROTO_OK                =  0,
    PROTO_ERR_INVALID_JSON  = -1,
    PROTO_ERR_NO_TYPE       = -2,
    PROTO_ERR_PARSE_FIELD   = -3,
    PROTO_ERR_UNKNOWN_TYPE  = -4,
} proto_result_t;

/* ---- ACOS V1 server event IDs -------------------------------------------- */
typedef enum {
    PROTO_EVT_UNKNOWN              = 0,
    /* auth / handshake */
    PROTO_EVT_PROXY_CONNECTED      = 1,  /* __proxy_connected__ */
    PROTO_EVT_ERROR                = 2,  /* error */
    /* VAD events from server */
    PROTO_EVT_SPEECH_STARTED       = 3,  /* input_audio_buffer.speech_started */
    PROTO_EVT_SPEECH_STOPPED       = 4,  /* input_audio_buffer.speech_stopped */
    /* AI reply lifecycle */
    PROTO_EVT_RESPONSE_DONE        = 5,  /* response.audio.done */
    PROTO_EVT_RESPONSE_TERMINAL    = 6,  /* response.done / explicit cancelled */
} proto_event_t;

/* ---- Parsed message view ------------------------------------------------ */
typedef struct {
    proto_event_t  evt;             /* mapped event id                       */
    const char    *raw_type;        /* raw type string from server           */
    int            raw_type_len;

    /* error frame */
    int            error_code;
    const char    *error_code_text;
    int            error_code_text_len;
    const char    *error_msg;
    int            error_msg_len;

    /* speech_started/stopped: optional session_id echoed by server */
    const char    *session_id;
    int            session_id_len;

    /* response.audio.done: optional reason */
    const char    *reason;
    int            reason_len;
    const char    *status;
    int            status_len;
} proto_msg_t;

/* ---- Public API --------------------------------------------------------- */

/**
 * @brief Parse one JSON text frame into msg.
 * @param json_str NUL-terminated JSON string.
 * @param msg      output, caller-owned.
 */
proto_result_t proto_parse_json(const char *json_str, proto_msg_t *msg);
typedef void (*proto_event_callback_t)(proto_event_t event);
/* Register before starting the WebSocket client. */
void proto_service_register_callback(proto_event_callback_t callback);
proto_result_t proto_dispatch_data(const char *data, size_t len);

/** No-op (struct holds only borrowed pointers). Kept for API symmetry. */
void proto_msg_clear(proto_msg_t *msg);

/**
 * @brief Build `auth` JSON frame (first message after WebSocket open).
 *
 *   Format: {"type":"auth","token":"<token>"}
 *
 * @param buf       output buffer
 * @param buf_size  buffer size
 * @param token     API Key (sk-...)
 * @return          length written (>=0), 0 on failure
 */
size_t proto_build_auth_frame(char *buf, size_t buf_size, const char *token);

/**
 * @brief Build `input_audio_buffer.append` JSON frame.
 *
 *   Format:
 *     {"event_id":"event_<ms>_<rand>","type":"input_audio_buffer.append",
 *      "audio":"<base64>","encoding":"opus"}
 *
 * @param buf           output buffer (must fit JSON + base64 string)
 * @param buf_size      buffer size
 * @param base64_audio  pre-encoded Base64 Opus payload (NUL-terminated)
 * @param event_id      optional event_id string (NULL = auto-generate)
 * @return              length written (>=0), 0 on failure
 */
size_t proto_build_input_audio_append(char *buf, size_t buf_size,
                                       const char *base64_audio,
                                       const char *event_id);

/**
 * @brief Helper: produce a unique event_id string.
 *   Format: "event_<ms>_<6hex>"
 *
 * @param buf       output buffer (>= 32 bytes)
 * @param buf_size  buffer size
 */
void proto_make_event_id(char *buf, size_t buf_size);

/**
 * @brief Map proto_event_t to human-readable name (debug only).
 */
const char *proto_event_name(proto_event_t evt);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_SERVICE_H */
