/*
 * Whether the session asks not to sleep or turn the display off: a video
 * playing, or the user's "do not suspend". The viewer holds the same on
 * Windows. Read from gnome-session, Plasma's PowerDevil and logind, whichever
 * are there; the session bus ones only report what their desktop was asked,
 * and logind what was asked of it directly (systemd-inhibit, other
 * compositors).
 */

#ifndef WEASELWAY_INHIBIT_H
#define WEASELWAY_INHIBIT_H

#include <stdint.h>

struct inhibit;

/* @changed gets the WWDISPLAY_INHIBIT_* flags whenever they change, on the
 * thread-default main context. They start at 0. */
struct inhibit *inhibit_new(void (*changed)(void *data, uint32_t flags), void *data);
void inhibit_free(struct inhibit *inhibit);

#endif
