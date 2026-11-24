/*
 * Weaselway::Display, the viewer's own channel (wwdisplay in weaselway's
 * FreeRDP): what RDP has no field for, about each monitor of the client's.
 * Every PDU is UINT16 type, UINT16 monitor, then the body, little endian.
 * The monitor is the index into the client's layout, which is the dxgdrm
 * output that shows it.
 *
 *   REFRESH_RATE    UINT32 refresh rate in millihertz
 *   PRESENT_TIMING  UINT32 frame count, INT32 slack in microseconds: how long
 *                   before the monitor's vblank the last frames were ready
 *
 * The one PDU the server sends, with monitor 0:
 *
 *   INHIBIT         UINT32 WWDISPLAY_INHIBIT_* flags: what the session asks
 *                   Windows to hold off, sent on every change
 */

#ifndef WEASELWAY_WWDISPLAY_H
#define WEASELWAY_WWDISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WWDISPLAY_DVC_CHANNEL_NAME "Weaselway::Display"
#define WWDISPLAY_PDU_REFRESH_RATE 1
#define WWDISPLAY_PDU_PRESENT_TIMING 2
#define WWDISPLAY_PDU_INHIBIT 3
#define WWDISPLAY_INHIBIT_SYSTEM 1  /* sleep */
#define WWDISPLAY_INHIBIT_DISPLAY 2 /* turning the display off */
#define WWDISPLAY_MAX_PDU 64

struct wwdisplay_pdu {
    unsigned type;
    unsigned monitor;
    uint32_t refresh_mhz;
    uint32_t frames;
    int32_t slack_us;
};

/* False for a PDU that is too short, or a known type of the wrong length. An
 * unknown type parses, with only @type and @monitor set: it is for a newer
 * client, and skipped. */
bool wwdisplay_parse(const uint8_t *data, size_t length, struct wwdisplay_pdu *pdu);

#endif
