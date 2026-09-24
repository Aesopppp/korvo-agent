/**
 * proto_service.c -- ACOS V1 protocol frame parser & builder.
 *
 *   Hand-written JSON parser (no cJSON). Parser only extracts fields we
 *   actually need; everything else is left untouched.
 *
 *   Frame builders are pure snprintf wrappers; Base64 encoding of Opus
 *   happens in ws_service.c (using mbedtls_base64_encode).
 */

#include "proto_service.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "proto_service";
static proto_event_callback_t s_callback;

void proto_service_register_callback(proto_event_callback_t callback)
{
    s_callback = callback;
}

proto_result_t proto_dispatch_data(const char *data, size_t len)
{
    if (!data || !len || len == SIZE_MAX) return PROTO_ERR_INVALID_JSON;
    char *text = malloc(len + 1);
    if (!text) return PROTO_ERR_INVALID_JSON;
    memcpy(text, data, len);
    text[len] = 0;
    proto_msg_t msg;
    proto_result_t result = proto_parse_json(text, &msg);
    if (result == PROTO_OK && s_callback) s_callback(msg.evt);
    free(text);
    return result;
}

/* ---- helpers ----------------------------------------------------------- */
static const char *skip_ws(const char *p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *parse_string_field(const char *p, const char *key, int *out_len)
{
    size_t key_len = strlen(key);
    const char *cur = p;
    while ((cur = strchr(cur, '"')) != NULL) {
        const char *after = cur + 1;
        if (strncmp(after, key, key_len) == 0 && after[key_len] == '"') {
            const char *colon = after + key_len + 1;
            while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t')) colon++;
            if (*colon != '"') { cur++; continue; }
            const char *vs = colon + 1;
            const char *ve = vs;
            while (*ve && *ve != '"') {
                if (*ve == '\\' && ve[1]) { ve += 2; continue; }
                ve++;
            }
            *out_len = (int)(ve - vs);
            return vs;
        }
        cur++;
    }
    return NULL;
}

static int parse_int_field(const char *p, const char *key, int default_val)
{
    size_t key_len = strlen(key);
    const char *cur = p;
    while ((cur = strchr(cur, '"')) != NULL) {
        const char *after = cur + 1;
        if (strncmp(after, key, key_len) == 0 && after[key_len] == '"') {
            const char *v = after + key_len + 1;
            while (*v && (*v == ':' || *v == ' ' || *v == '\t')) v++;
            if (isdigit((unsigned char)*v) || *v == '-') return atoi(v);
            return default_val;
        }
        cur++;
    }
    return default_val;
}

static int type_equals(const char *s, size_t s_len, const char *literal)
{
    size_t n = strlen(literal);
    return s_len == n && strncmp(s, literal, n) == 0;
}

/* ---- parser ------------------------------------------------------------ */
proto_result_t proto_parse_json(const char *json_str, proto_msg_t *msg)
{
    if (!json_str || !msg) return PROTO_ERR_INVALID_JSON;
    memset(msg, 0, sizeof(*msg));
    msg->evt = PROTO_EVT_UNKNOWN;

    int type_len = 0;
    const char *type_val = parse_string_field(json_str, "type", &type_len);
    if (!type_val || type_len == 0) return PROTO_ERR_NO_TYPE;

    msg->raw_type    = type_val;
    msg->raw_type_len = type_len;

    if (type_equals(type_val, type_len, "__proxy_connected__")) {
        msg->evt = PROTO_EVT_PROXY_CONNECTED;
        msg->session_id = parse_string_field(json_str, "session_id", &msg->session_id_len);
    } else if (type_equals(type_val, type_len, "input_audio_buffer.speech_started")) {
        msg->evt = PROTO_EVT_SPEECH_STARTED;
    } else if (type_equals(type_val, type_len, "input_audio_buffer.speech_stopped")) {
        msg->evt = PROTO_EVT_SPEECH_STOPPED;
    } else if (type_equals(type_val, type_len, "response.audio.done")) {
        msg->evt = PROTO_EVT_RESPONSE_DONE;
        msg->reason = parse_string_field(json_str, "reason", &msg->reason_len);
    } else if (type_equals(type_val, type_len, "response.done")) {
        msg->evt = PROTO_EVT_RESPONSE_TERMINAL;
        msg->status = parse_string_field(json_str, "status", &msg->status_len);
        msg->reason = parse_string_field(json_str, "reason", &msg->reason_len);
    } else if (type_equals(type_val, type_len, "response.cancelled") ||
               type_equals(type_val, type_len, "response.canceled")) {
        msg->evt = PROTO_EVT_RESPONSE_TERMINAL;
        msg->status = "cancelled";
        msg->status_len = 9;
    } else if (type_equals(type_val, type_len, "error")) {
        msg->evt = PROTO_EVT_ERROR;
        msg->error_code = parse_int_field(json_str, "code", -1);
        msg->error_code_text = parse_string_field(json_str, "code", &msg->error_code_text_len);
        msg->error_msg  = parse_string_field(json_str, "message", &msg->error_msg_len);
    } else {
        ESP_LOGD(TAG, "unknown frame type: %.*s", type_len, type_val);
        return PROTO_ERR_UNKNOWN_TYPE;
    }
    return PROTO_OK;
}

void proto_msg_clear(proto_msg_t *msg) { (void)msg; }

/* ---- builders ---------------------------------------------------------- */
size_t proto_build_auth_frame(char *buf, size_t buf_size, const char *token)
{
    if (!buf || !token) return 0;
    int n = snprintf(buf, buf_size,
                     "{\"type\":\"auth\",\"token\":\"%s\"}",
                     token);
    return (n > 0 && (size_t)n < buf_size) ? (size_t)n : 0;
}

size_t proto_build_input_audio_append(char *buf, size_t buf_size,
                                       const char *base64_audio,
                                       const char *event_id)
{
    if (!buf || !base64_audio) return 0;

    char auto_id[32];
    const char *eid = event_id;
    if (!eid || eid[0] == '\0') {
        proto_make_event_id(auto_id, sizeof(auto_id));
        eid = auto_id;
    }
    int n = snprintf(buf, buf_size,
                     "{\"event_id\":\"%s\","
                     "\"type\":\"input_audio_buffer.append\","
                     "\"audio\":\"%s\","
                     "\"encoding\":\"opus\"}",
                     eid, base64_audio);
    return (n > 0 && (size_t)n < buf_size) ? (size_t)n : 0;
}

void proto_make_event_id(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 24) return;
    uint64_t ms = (uint64_t)(esp_timer_get_time() / 1000);
    uint32_t rnd = esp_random();
    snprintf(buf, buf_size, "event_%llu_%06lx",
             (unsigned long long)ms, (unsigned long)(rnd & 0xFFFFFFu));
}

const char *proto_event_name(proto_event_t evt)
{
    switch (evt) {
        case PROTO_EVT_PROXY_CONNECTED: return "PROXY_CONNECTED";
        case PROTO_EVT_ERROR:           return "ERROR";
        case PROTO_EVT_SPEECH_STARTED:  return "SPEECH_STARTED";
        case PROTO_EVT_SPEECH_STOPPED:  return "SPEECH_STOPPED";
        case PROTO_EVT_RESPONSE_DONE:   return "RESPONSE_DONE";
        case PROTO_EVT_RESPONSE_TERMINAL:return "RESPONSE_TERMINAL";
        default:                        return "UNKNOWN";
    }
}
