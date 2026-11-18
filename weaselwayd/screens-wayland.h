/*
 * Where the compositor put dxgdrm's outputs: their logical position and size,
 * from xdg-output. The compositor maps weaselwayd's absolute pointer onto the
 * bounding box of these. They differ from the client's monitors as soon as an
 * output is scaled, or arranged other than the client's monitors are.
 */

#ifndef WEASELWAY_SCREENS_WAYLAND_H
#define WEASELWAY_SCREENS_WAYLAND_H

#include <stdbool.h>

#include "layout.h"

struct wayland_screens;

struct wayland_screens *wayland_screens_new(void);
void wayland_screens_free(struct wayland_screens *screens);

/* The logical rect of dxgdrm output @index. False while the compositor has
 * not said, or does not show it. */
bool wayland_screens_get(struct wayland_screens *screens, int index, struct monitor *screen);

#endif
