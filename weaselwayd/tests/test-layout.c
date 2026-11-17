/*
 * The monitor layout math and the Weaselway::Display parser, which need
 * nothing to run. `make check` runs it.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "layout.h"
#include "wwdisplay.h"

static void
test_normalize(void)
{
    /* Primary at 0,0 and one to its left, as Windows numbers them. */
    struct monitor m[2] = {
        { .x = 0, .y = 0, .width = 1920, .height = 1080 },
        { .x = -1280, .y = 200, .width = 1280, .height = 720 },
    };
    struct monitor none = { .width = 0, .height = 100 };
    int w, h;

    assert(layout_normalize(m, 2));
    assert(m[0].x == 1280 && m[0].y == 0);
    assert(m[1].x == 0 && m[1].y == 200);
    layout_extent(m, 2, &w, &h);
    assert(w == 3200 && h == 1080);

    assert(!layout_normalize(m, 0));
    assert(!layout_normalize(m, LAYOUT_MAX_MONITORS + 1));
    assert(!layout_normalize(&none, 1));
}

static void
test_monitor_at(void)
{
    struct monitor m[2] = {
        { .x = 0, .y = 0, .width = 100, .height = 100 },
        { .x = 100, .y = 0, .width = 50, .height = 50 },
    };

    assert(layout_monitor_at(m, 2, 0, 0) == 0);
    assert(layout_monitor_at(m, 2, 99, 99) == 0);
    assert(layout_monitor_at(m, 2, 100, 0) == 1);
    assert(layout_monitor_at(m, 2, 149, 49) == 1);
    /* Below the small one, in the corner of the bounding box: nearest. */
    assert(layout_monitor_at(m, 2, 145, 70) == 1);
    assert(layout_monitor_at(m, 2, -5, 50) == 0);
    assert(layout_monitor_at(m, 0, 0, 0) == -1);
}

static void
test_map_point(void)
{
    struct monitor m[2] = {
        { .x = 0, .y = 0, .width = 1000, .height = 500 },
        { .x = 1000, .y = 0, .width = 500, .height = 500 },
    };
    /* The second screen has not followed a resize yet: twice as large. */
    struct monitor s[2] = {
        { .x = 0, .y = 0, .width = 1000, .height = 500 },
        { .x = 1000, .y = 0, .width = 1000, .height = 1000 },
    };
    int x, y;

    /* Same size: the point stays where it is. */
    assert(layout_map_point(m, m, 2, 1200, 300, &x, &y) == 1);
    assert(x == 1200 && y == 300);
    assert(layout_map_point(m, s, 2, 10, 20, &x, &y) == 0);
    assert(x == 10 && y == 20);

    /* Scaled onto the larger screen, last pixel to last pixel. */
    assert(layout_map_point(m, s, 2, 1499, 499, &x, &y) == 1);
    assert(x == 1999 && y == 999);
    assert(layout_map_point(m, s, 2, 1000, 0, &x, &y) == 1);
    assert(x == 1000 && y == 0);

    assert(layout_map_point(m, s, 0, 1, 1, &x, &y) == -1);
}

static void
test_wwdisplay(void)
{
    const uint8_t refresh[] = { 1, 0, 2, 0, 0xa0, 0xea, 0x02, 0x00 }; /* 191 Hz, monitor 2 */
    const uint8_t timing[] = { 2, 0, 1, 0, 60, 0, 0, 0, 0x18, 0xfc, 0xff, 0xff };
    const uint8_t unknown[] = { 9, 0, 0, 0, 1, 2, 3 };
    struct wwdisplay_pdu pdu;

    assert(wwdisplay_parse(refresh, sizeof(refresh), &pdu));
    assert(pdu.type == WWDISPLAY_PDU_REFRESH_RATE && pdu.monitor == 2);
    assert(pdu.refresh_mhz == 191136);

    assert(wwdisplay_parse(timing, sizeof(timing), &pdu));
    assert(pdu.type == WWDISPLAY_PDU_PRESENT_TIMING && pdu.monitor == 1);
    assert(pdu.frames == 60 && pdu.slack_us == -1000);

    assert(wwdisplay_parse(unknown, sizeof(unknown), &pdu));
    assert(pdu.type == 9);

    assert(!wwdisplay_parse(refresh, 3, &pdu));
    assert(!wwdisplay_parse(refresh, 7, &pdu));
    assert(!wwdisplay_parse(timing, 8, &pdu));
}

int
main(void)
{
    test_normalize();
    test_monitor_at();
    test_map_point();
    test_wwdisplay();
    printf("test-layout: ok\n");
    return 0;
}
