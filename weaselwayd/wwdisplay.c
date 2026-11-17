/* See wwdisplay.h. */

#include "wwdisplay.h"

static uint32_t
le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool
wwdisplay_parse(const uint8_t *data, size_t length, struct wwdisplay_pdu *pdu)
{
    if (length < 4)
        return false;

    *pdu = (struct wwdisplay_pdu){
        .type = data[0] | data[1] << 8,
        .monitor = data[2] | data[3] << 8,
    };

    switch (pdu->type) {
    case WWDISPLAY_PDU_REFRESH_RATE:
        if (length != 8)
            return false;
        pdu->refresh_mhz = le32(data + 4);
        return true;
    case WWDISPLAY_PDU_PRESENT_TIMING:
        if (length != 12)
            return false;
        pdu->frames = le32(data + 4);
        pdu->slack_us = (int32_t)le32(data + 8);
        return true;
    default:
        return true;
    }
}
