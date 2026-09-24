#include "proto_service.h"
#include <string.h>
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void)
{
    proto_msg_t msg;
    CHECK(proto_parse_json("{\"type\":\"response.audio.done\"}", &msg) == PROTO_OK);
    CHECK(msg.evt == PROTO_EVT_RESPONSE_DONE);
    CHECK(proto_parse_json("{\"type\":\"response.done\",\"response\":{\"status\":\"cancelled\"}}", &msg) == PROTO_OK);
    CHECK(msg.evt == PROTO_EVT_RESPONSE_TERMINAL);
    CHECK(msg.status_len == 9 && memcmp(msg.status, "cancelled", 9) == 0);
    CHECK(proto_parse_json("{\"type\":\"response.done\",\"response\":{\"status\":\"completed\"}}", &msg) == PROTO_OK);
    CHECK(msg.evt == PROTO_EVT_RESPONSE_TERMINAL);
    CHECK(msg.status_len == 9 && memcmp(msg.status, "completed", 9) == 0);
    CHECK(proto_parse_json("{\"type\":\"response.cancelled\"}", &msg) == PROTO_OK);
    CHECK(msg.evt == PROTO_EVT_RESPONSE_TERMINAL && msg.status_len == 9);
    CHECK(proto_parse_json("{\"type\":\"response.canceled\"}", &msg) == PROTO_OK);
    CHECK(msg.evt == PROTO_EVT_RESPONSE_TERMINAL);
    CHECK(proto_parse_json("{\"type\":\"response.done\",\"response\":{\"status\":\"failed\"}}", &msg) == PROTO_OK);
    CHECK(msg.status_len == 6 && memcmp(msg.status, "failed", 6) == 0);
    CHECK(proto_parse_json("{\"type\":\"response.audio.delta\"}", &msg) == PROTO_ERR_UNKNOWN_TYPE);
    CHECK(msg.evt == PROTO_EVT_UNKNOWN);
    return 0;
}
