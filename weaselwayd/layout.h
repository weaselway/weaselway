/*
 * The client's monitors and the screens that show them, as rectangles in one
 * plane, for mapping the client's pointer onto the screens.
 *
 * Monitor i of the client's layout is shown by dxgdrm output i. A monitor is
 * where the client's window is, in the client's desktop; the screen is where
 * the compositor put the output in its logical layout (screens-wayland.c), or,
 * until it has said, at the monitor's position (dxgdrm's suggested X/Y) with
 * the size of the compositor's mode.
 */

#ifndef WEASELWAY_LAYOUT_H
#define WEASELWAY_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* dxgdrm's DXGDRM_MAX_OUTPUTS; weaselwayd.c checks that they match. */
#define LAYOUT_MAX_MONITORS 4

struct monitor {
    int x, y, width, height;
    /* The physical size, 0 if unknown. */
    uint32_t width_mm, height_mm;
    /* The refresh rate, 0 until the client says. */
    uint32_t refresh_mhz;
};

/* Moves the monitors so that the top-left of their bounding box is at 0,0,
 * which is where the client's desktop starts. False, and nothing moved, if
 * there are none, too many, or one has no area. */
bool layout_normalize(struct monitor *monitors, int count);

/* The size of the bounding box of the rects, from 0,0. */
void layout_extent(const struct monitor *rects, int count, int *width, int *height);

/* The monitor a point of the client's desktop is on, or the nearest one if it
 * is on none; -1 if there are no monitors. */
int layout_monitor_at(const struct monitor *monitors, int count, int x, int y);

/*
 * A point of the client's desktop as a point among the screens: on the
 * monitor it is on, scaled to that monitor's screen, at the screen's position.
 * @screens[i] shows @monitors[i]. Returns the monitor, -1 if there is none.
 */
int layout_map_point(const struct monitor *monitors, const struct monitor *screens, int count,
                     int x, int y, int *screen_x, int *screen_y);

#endif
