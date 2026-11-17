/* See layout.h. */

#include "layout.h"

#include <limits.h>

bool
layout_normalize(struct monitor *monitors, int count)
{
    int min_x = INT_MAX, min_y = INT_MAX;

    if (count <= 0 || count > LAYOUT_MAX_MONITORS)
        return false;

    for (int i = 0; i < count; i++) {
        if (monitors[i].width <= 0 || monitors[i].height <= 0)
            return false;
        min_x = monitors[i].x < min_x ? monitors[i].x : min_x;
        min_y = monitors[i].y < min_y ? monitors[i].y : min_y;
    }

    for (int i = 0; i < count; i++) {
        monitors[i].x -= min_x;
        monitors[i].y -= min_y;
    }
    return true;
}

void
layout_extent(const struct monitor *rects, int count, int *width, int *height)
{
    *width = *height = 0;
    for (int i = 0; i < count; i++) {
        int x2 = rects[i].x + rects[i].width, y2 = rects[i].y + rects[i].height;

        *width = x2 > *width ? x2 : *width;
        *height = y2 > *height ? y2 : *height;
    }
}

/* How far a coordinate is outside [start, start + size). */
static long long
outside(int v, int start, int size)
{
    if (v < start)
        return (long long)start - v;
    if (v >= start + size)
        return (long long)v - (start + size - 1);
    return 0;
}

int
layout_monitor_at(const struct monitor *monitors, int count, int x, int y)
{
    long long best_distance = -1;
    int best = -1;

    for (int i = 0; i < count; i++) {
        long long dx = outside(x, monitors[i].x, monitors[i].width);
        long long dy = outside(y, monitors[i].y, monitors[i].height);
        long long distance = dx * dx + dy * dy;

        if (best < 0 || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

/* @v from [0, @from) to [0, @to), keeping the last pixel the last. */
static int
scale(int v, int from, int to)
{
    v = v < 0 ? 0 : v >= from ? from - 1 : v;
    if (from < 2 || to < 2)
        return 0;
    return (int)((long long)v * (to - 1) / (from - 1));
}

int
layout_map_point(const struct monitor *monitors, const struct monitor *screens, int count,
                 int x, int y, int *screen_x, int *screen_y)
{
    int i = layout_monitor_at(monitors, count, x, y);

    if (i < 0)
        return -1;
    *screen_x = screens[i].x + scale(x - monitors[i].x, monitors[i].width, screens[i].width);
    *screen_y = screens[i].y + scale(y - monitors[i].y, monitors[i].height, screens[i].height);
    return i;
}
