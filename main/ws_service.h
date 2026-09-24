/**
 * ws_service.h -- ACOS V1 WebSocket client.
 *
 *   Responsibilities:
 *     - Open / maintain / auto-reconnect WebSocket (WSS).
 *     - WebSocket Ping/Pong keepalive (handled by esp_websocket_client).
 *     - Auth handshake: send `auth` JSON on connect, wait for `__proxy_connected__`.
 *     - Upload: Opus frames -> Base64 -> `input_audio_buffer.append` text frames.
 *     - Download: raw Opus binary frames -> decode -> audio_out_play_opus().
 *     - Server events: speech_started / speech_stopped / response.audio.done / error.
 *     - Barge-In: speech_started triggers audio_out_stop() (caller clears queue).
 *
 *   Spec target:
 *     WSS://acos-platform.emicloud.com/acos-realtime
 *     (overridable via CFG_KEY_WS_URL or NVS)
 *
 *   Thread-safety:
 *     All public APIs are safe to call from main / FreeRTOS tasks.
 *     WebSocket events are dispatched via esp_websocket event handler
 *     (runs inside esp_websocket_client internal task) and forwarded to
 *     app_event_queue / event_bus.
 */

#ifndef WS_SERVICE_H
#define WS_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_websocket_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- WebSocket connection state ---------------------------------------- */
typedef enum {
    WS_STATE_DISCONNECTED  = 0,
    WS_STATE_CONNECTING    = 1,  /* TCP/TLS handshaking */
    WS_STATE_AUTHING       = 2,  /* WS opened, auth sent, awaiting __proxy_connected__ */
    WS_STATE_CONNECTED     = 3,  /* auth ok, ready to send/receive audio */
    WS_STATE_ERROR         = 4,
} ws_conn_state_t;

/* ---- Internal service event types (forwarded to app_event_queue) ----- */
typedef enum {
    WS_SERVICE_EVENT_CONNECTED        = 0,  /* TCP/TLS connected */
    WS_SERVICE_EVENT_AUTH_OK          = 1,  /* __proxy_connected__ received */
    WS_SERVICE_EVENT_AUTH_FAIL        = 2,  /* server rejected auth */
    WS_SERVICE_EVENT_DISCONNECTED     = 3,
    WS_SERVICE_EVENT_ERROR            = 4,
    WS_SERVICE_EVENT_AUDIO_FRAME_IN   = 5,  /* raw Opus received */
    WS_SERVICE_EVENT_SPEECH_STARTED   = 6,  /* server VAD: user started talking */
    WS_SERVICE_EVENT_SPEECH_STOPPED   = 7,  /* server VAD: user stopped talking */
    WS_SERVICE_EVENT_RESPONSE_DONE    = 8,  /* AI reply finished */
    WS_SERVICE_EVENT_RECONNECTING     = 9,
} ws_service_event_type_t;

typedef struct {
    ws_service_event_type_t type;
    const uint8_t          *audio_payload; /* AUDIO_FRAME_IN: borrowed ptr */
    size_t                  audio_len;
    const char             *error_msg;     /* ERROR / AUTH_FAIL */
} ws_service_event_data_t;

/* ---- Public API -------------------------------------------------------- */
esp_err_t ws_service_init(void);
esp_err_t ws_service_connect(void);
esp_err_t ws_service_disconnect(void);
esp_err_t ws_service_deinit(void);

/**
 * @brief Send one Opus frame as a Base64-encoded JSON text frame.
 *
 *   Internally: Opus -> mbedtls_base64_encode -> JSON -> WS text frame.
 *
 * @param opus_data  Opus payload (raw bytes from opus_enc_encode)
 * @param opus_len   payload length in bytes
 * @return ESP_OK on enqueue; ESP_ERR_INVALID_STATE if WS not CONNECTED.
 */
esp_err_t ws_service_send_opus_frame(const uint8_t *opus_data, size_t opus_len);
esp_err_t ws_service_cancel_response(void);
/** Main-task-only lifecycle APIs. RX callbacks never wait for audio tasks. */
esp_err_t ws_service_authenticate(void);
uint32_t ws_service_generation(void);
void ws_service_log_cancel_wait(void);

/** Generic JSON text send (for future protocol extensions). */
esp_err_t ws_service_send_json(const char *json_str);

ws_conn_state_t ws_service_get_state(void);
esp_websocket_client_handle_t ws_service_get_client(void);

#ifdef __cplusplus
}
#endif

#endif /* WS_SERVICE_H */
