/*
 * See screens-wayland.h. The compositor is found by wayland-watch.c.
 *
 * DRM names connectors by type and a number per type that counts from 1 in
 * the order they were created, and dxgdrm creates them in output order: output
 * i is "Virtual-<i+1>". wl_output.name (version 4) carries that name in
 * mutter, KWin, Hyprland and niri; xdg_output.name is the fallback.
 *
 * shortcut: the first compositor with xdg-output is taken, even one that does
 * not show dxgdrm (a nested one), which leaves the pointer on the client's
 * layout; skip compositors without "Virtual-" outputs if that ever matters.
 */

#define G_LOG_DOMAIN "screens"

#include "screens-wayland.h"

#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <wayland-client.h>

#include "wayland-watch.h"
#include "xdg-output-unstable-v1-client-protocol.h"

struct screen {
    struct wayland_screens *screens;
    uint32_t global;
    struct wl_output *output;
    struct zxdg_output_v1 *xdg;

    /* As of the last done: the dxgdrm output (-1 for none) and its rect. */
    int index;
    struct monitor rect;
    bool have_rect;

    /* Since the last done. */
    char *name;
    struct monitor pending;
    bool have_position, have_size;
};

struct wayland_screens {
    struct wayland_watch *watch;
    struct zxdg_output_manager_v1 *manager;
    GPtrArray *list;
};

static int
index_of(const char *name)
{
    int n, end = 0;

    if (!name || sscanf(name, "Virtual-%d%n", &n, &end) != 1 || name[end] || n < 1 ||
        n > LAYOUT_MAX_MONITORS)
        return -1;
    return n - 1;
}

/* The compositor's state of @screen is complete. */
static void
apply(struct screen *screen)
{
    int index = index_of(screen->name);

    if (!screen->have_position || !screen->have_size)
        return;
    if (index == screen->index && screen->have_rect &&
        !memcmp(&screen->rect, &screen->pending, sizeof(screen->rect)))
        return;

    screen->index = index;
    screen->rect = screen->pending;
    screen->have_rect = true;
    if (index >= 0)
        g_message("output %d (%s) is at %d,%d, %dx%d in the compositor's layout", index,
                  screen->name, screen->rect.x, screen->rect.y, screen->rect.width,
                  screen->rect.height);
}

static void
on_xdg_position(void *data, struct zxdg_output_v1 *xdg, int32_t x, int32_t y)
{
    struct screen *screen = data;

    screen->pending.x = x;
    screen->pending.y = y;
    screen->have_position = true;
}

static void
on_xdg_size(void *data, struct zxdg_output_v1 *xdg, int32_t width, int32_t height)
{
    struct screen *screen = data;

    screen->pending.width = width;
    screen->pending.height = height;
    screen->have_size = true;
}

/* Up to version 2; from 3 on, wl_output.done is sent instead. */
static void
on_xdg_done(void *data, struct zxdg_output_v1 *xdg)
{
    apply(data);
}

static void
on_xdg_name(void *data, struct zxdg_output_v1 *xdg, const char *name)
{
    struct screen *screen = data;

    /* wl_output.name, where there is one, comes first and wins. */
    if (!screen->name)
        screen->name = g_strdup(name);
}

static void
on_xdg_description(void *data, struct zxdg_output_v1 *xdg, const char *description)
{
}

static const struct zxdg_output_v1_listener xdg_listener = {
    .logical_position = on_xdg_position,
    .logical_size = on_xdg_size,
    .done = on_xdg_done,
    .name = on_xdg_name,
    .description = on_xdg_description,
};

static void
on_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t width_mm,
            int32_t height_mm, int32_t subpixel, const char *make, const char *model,
            int32_t transform)
{
}

static void
on_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height,
        int32_t refresh)
{
}

static void
on_done(void *data, struct wl_output *output)
{
    apply(data);
}

static void
on_scale(void *data, struct wl_output *output, int32_t factor)
{
}

static void
on_name(void *data, struct wl_output *output, const char *name)
{
    struct screen *screen = data;

    g_free(screen->name);
    screen->name = g_strdup(name);
}

static void
on_description(void *data, struct wl_output *output, const char *description)
{
}

static const struct wl_output_listener output_listener = {
    .geometry = on_geometry,
    .mode = on_mode,
    .done = on_done,
    .scale = on_scale,
    .name = on_name,
    .description = on_description,
};

static void
watch_xdg(struct wayland_screens *screens, struct screen *screen)
{
    if (!screens->manager || screen->xdg)
        return;
    screen->xdg = zxdg_output_manager_v1_get_xdg_output(screens->manager, screen->output);
    zxdg_output_v1_add_listener(screen->xdg, &xdg_listener, screen);
}

static void
free_screen(gpointer data)
{
    struct screen *screen = data;

    g_clear_pointer(&screen->xdg, zxdg_output_v1_destroy);
    if (wl_output_get_version(screen->output) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
        wl_output_release(screen->output);
    else
        wl_output_destroy(screen->output);
    g_free(screen->name);
    g_free(screen);
}

static void
on_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
          uint32_t version)
{
    struct wayland_screens *screens = data;

    if (!strcmp(interface, wl_output_interface.name)) {
        struct screen *screen = g_new0(struct screen, 1);

        screen->screens = screens;
        screen->global = name;
        screen->index = -1;
        screen->output = wl_registry_bind(registry, name, &wl_output_interface, MIN(version, 4));
        wl_output_add_listener(screen->output, &output_listener, screen);
        watch_xdg(screens, screen);
        g_ptr_array_add(screens->list, screen);
    } else if (!strcmp(interface, zxdg_output_manager_v1_interface.name) && !screens->manager) {
        screens->manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface,
                                            MIN(version, 3));
        for (guint i = 0; i < screens->list->len; i++)
            watch_xdg(screens, g_ptr_array_index(screens->list, i));
    }
}

static void
on_global_remove(void *data, uint32_t name)
{
    struct wayland_screens *screens = data;

    for (guint i = 0; i < screens->list->len; i++) {
        struct screen *screen = g_ptr_array_index(screens->list, i);

        if (screen->global == name) {
            if (screen->index >= 0)
                g_message("output %d (%s) left the compositor's layout", screen->index,
                          screen->name);
            g_ptr_array_remove_index(screens->list, i);
            return;
        }
    }
}

static void
on_ready(void *data)
{
    struct wayland_screens *screens = data;

    if (!screens->manager) {
        g_debug("%s has no xdg-output", wayland_watch_socket(screens->watch));
        wayland_watch_drop(screens->watch, true);
        return;
    }
    g_message("following the screen layout of %s", wayland_watch_socket(screens->watch));
}

static void
on_disconnected(void *data)
{
    struct wayland_screens *screens = data;

    g_ptr_array_set_size(screens->list, 0);
    g_clear_pointer(&screens->manager, zxdg_output_manager_v1_destroy);
}

static const struct wayland_watch_handler watch_handler = {
    .global = on_global,
    .global_remove = on_global_remove,
    .ready = on_ready,
    .disconnected = on_disconnected,
};

struct wayland_screens *
wayland_screens_new(void)
{
    struct wayland_screens *screens = g_new0(struct wayland_screens, 1);

    screens->list = g_ptr_array_new_with_free_func(free_screen);
    screens->watch = wayland_watch_new("screens", &watch_handler, screens);
    return screens;
}

void
wayland_screens_free(struct wayland_screens *screens)
{
    if (!screens)
        return;

    wayland_watch_free(screens->watch);
    g_ptr_array_unref(screens->list);
    g_free(screens);
}

bool
wayland_screens_get(struct wayland_screens *screens, int index, struct monitor *rect)
{
    for (guint i = 0; i < screens->list->len; i++) {
        struct screen *screen = g_ptr_array_index(screens->list, i);

        if (screen->index == index && screen->have_rect) {
            *rect = screen->rect;
            return true;
        }
    }
    return false;
}
