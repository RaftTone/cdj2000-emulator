/* Test the actual built-source live frame-cache take function in isolation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BFIN_LINK_SIZES 1
struct bfin_sport {
    unsigned long long link_native_order[1][2], link_native_next;
    unsigned link_frame_len[1], link_frame_get[1], link_frame_put[1];
    int link_frame_fresh[1][2];
    unsigned char link_frame[1][2][512], link_last[64];
    unsigned link_wire_w29, link_wire_w30, link_rx_repeat_payload;
    int link_take_fresh, link_announce_pending;
};
static struct { unsigned armed, stale, hit, hit_announced, absent; } bfin_link_census[1];
static int bfin_sport_link_census(void) { return 0; }
static int bfin_link_census_slot(unsigned size) { (void)size; return 0; }
static unsigned bfin_sport_link_depth(void) { return 2; }
static int bfin_sport_link_repeat_announced(void) { return 1; }
#include "bfin-link-take.inc"

int main(int argc, char **argv)
{
    assert(argc == 2);
    int fresh_only = atoi(argv[1]);
    struct bfin_sport s = {0};
    unsigned char dest[64];
    s.link_frame_len[0] = 64;
    s.link_frame_fresh[0][0] = 1;
    s.link_frame_put[0] = 1;
    memset(s.link_frame[0][0], 0x33, 64);
    assert(bfin_sport_link_take(&s, dest, 64) == 64);
    assert(dest[0] == 0x33 && s.link_take_fresh);
    assert(bfin_sport_link_take(&s, dest, 64) == (fresh_only ? 0 : 64));
    s.link_frame_fresh[0][1] = 1;
    s.link_frame_put[0] = 0;
    memset(s.link_frame[0][1], 0x55, 64);
    assert(bfin_sport_link_take(&s, dest, 64) == 64);
    assert(dest[0] == 0x55 && s.link_take_fresh);
    assert(bfin_sport_link_take(&s, dest, 48) == 0);
    s.link_frame_len[0] = 48;
    s.link_wire_w29 = 1;
    s.link_wire_w30 = 24;
    assert(bfin_sport_link_take(&s, dest, 48) == (fresh_only ? 0 : 48));
    assert(s.link_rx_repeat_payload == (fresh_only ? 0u : 1u));
    return 0;
}
