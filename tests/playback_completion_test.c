#include "response_flow.h"
#define CHECK(c) do { if (!(c)) return __LINE__; } while(0)
int main(void)
{
    response_phase_t phase=RESPONSE_IDLE;
    response_expect(&phase);
    CHECK(response_accept_audio(&phase));
    CHECK(!response_playback_finished(phase,false,true));
    CHECK(!response_playback_finished(phase,true,false));
    CHECK(response_playback_finished(phase,true,true));
    CHECK(response_finish(&phase)==RESPONSE_END_NORMAL);
    CHECK(!response_playback_finished(phase,true,true));
    /* Cancel while the server is finished but PCM is still queued. */
    response_expect(&phase);
    CHECK(response_accept_audio(&phase));
    CHECK(!response_playback_finished(phase,true,false));
    CHECK(response_begin_cancel(&phase));
    CHECK(!response_playback_finished(phase,true,true));
    CHECK(response_finish(&phase)==RESPONSE_END_CANCELLED);
    CHECK(!response_playback_finished(phase,true,true));
    CHECK(!response_accept_audio(&phase));
    /* Empty/short next reply still completes once on its actual boundary. */
    response_expect(&phase);
    CHECK(!response_playback_finished(phase,false,true));
    CHECK(response_playback_finished(phase,true,true));
    CHECK(response_finish(&phase)==RESPONSE_END_NORMAL);
    return 0;
}
