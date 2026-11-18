/*
 * See wayland-watch.h.
 */

#define G_LOG_DOMAIN "wayland"

#include "wayland-watch.h"

#include <errno.h>
#include <string.h>

#include <gio/gio.h>
#include <glib-unix.h>

/* How long to wait for a socket that refused the connection. */
#define RETRY_SECONDS 3

struct wayland_watch {
    char *what;
    const struct wayland_watch_handler *handler;
    void *data;

    char *runtime_dir;
    GFileMonitor *monitor;
    guint retry_id;
    /* Sockets whose compositor was of no use, until they go. */
    GHashTable *rejected;
    /* Said once that there is no compositor yet. */
    bool complained;

    /* The compositor, once a socket answered. */
    char *socket;
    struct wl_display *display;
    guint display_id;
    struct wl_registry *registry;
    struct wl_callback *sync;
    /* A handler found the connection to be of no use; dropped after dispatch. */
    bool drop;
};

static void scan(struct wayland_watch *watch);

static void
disconnect(struct wayland_watch *watch)
{
    if (watch->display)
        watch->handler->disconnected(watch->data);

    g_clear_handle_id(&watch->display_id, g_source_remove);
    g_clear_pointer(&watch->sync, wl_callback_destroy);
    g_clear_pointer(&watch->registry, wl_registry_destroy);
    g_clear_pointer(&watch->display, wl_display_disconnect);
    g_clear_pointer(&watch->socket, g_free);
    watch->drop = false;
}

static void
on_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
          uint32_t version)
{
    struct wayland_watch *watch = data;

    watch->handler->global(watch->data, registry, name, interface, version);
}

static void
on_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    struct wayland_watch *watch = data;

    if (watch->handler->global_remove)
        watch->handler->global_remove(watch->data, name);
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

/* The compositor has told us its globals. */
static void
on_sync(void *data, struct wl_callback *callback, uint32_t time)
{
    struct wayland_watch *watch = data;

    g_clear_pointer(&watch->sync, wl_callback_destroy);
    watch->complained = false;
    watch->handler->ready(watch->data);
}

static const struct wl_callback_listener sync_listener = {
    .done = on_sync,
};

static gboolean
on_display(int fd, GIOCondition condition, gpointer data)
{
    struct wayland_watch *watch = data;

    if (!(condition & G_IO_IN) || wl_display_dispatch(watch->display) < 0) {
        g_message("%s: lost the connection to %s", watch->what, watch->socket);
        watch->drop = true;
    }

    if (watch->drop) {
        /* The source is gone with the return value. */
        watch->display_id = 0;
        disconnect(watch);
        scan(watch);
        return G_SOURCE_REMOVE;
    }

    wayland_watch_flush(watch);
    return G_SOURCE_CONTINUE;
}

static gboolean
on_retry(gpointer data)
{
    struct wayland_watch *watch = data;

    watch->retry_id = 0;
    scan(watch);
    return G_SOURCE_REMOVE;
}

/* Connect to @name and ask for its globals. False if nobody answers. */
static bool
try_socket(struct wayland_watch *watch, const char *name)
{
    g_autofree char *path = g_build_filename(watch->runtime_dir, name, NULL);

    watch->display = wl_display_connect(path);
    if (!watch->display) {
        g_debug("%s: cannot connect to %s: %s", watch->what, name, g_strerror(errno));
        return false;
    }

    watch->socket = g_strdup(name);
    watch->registry = wl_display_get_registry(watch->display);
    wl_registry_add_listener(watch->registry, &registry_listener, watch);
    watch->sync = wl_display_sync(watch->display);
    wl_callback_add_listener(watch->sync, &sync_listener, watch);
    watch->display_id = g_unix_fd_add(wl_display_get_fd(watch->display),
                                      G_IO_IN | G_IO_HUP | G_IO_ERR, on_display, watch);
    wayland_watch_flush(watch);
    return true;
}

static bool
is_socket_name(const char *name)
{
    return g_str_has_prefix(name, "wayland-") && !g_str_has_suffix(name, ".lock");
}

/* Look for a compositor, unless there is one. */
static void
scan(struct wayland_watch *watch)
{
    g_autoptr(GDir) dir = NULL;
    bool refused = false;
    const char *name;

    if (watch->display)
        return;

    dir = g_dir_open(watch->runtime_dir, 0, NULL);
    while (dir && (name = g_dir_read_name(dir))) {
        if (!is_socket_name(name) || g_hash_table_contains(watch->rejected, name))
            continue;
        if (try_socket(watch, name))
            return;
        refused = true;
    }

    /* Bound, but not yet listening. */
    if (refused && !watch->retry_id)
        watch->retry_id = g_timeout_add_seconds(RETRY_SECONDS, on_retry, watch);

    if (!watch->complained)
        g_debug("%s: no compositor for it in %s yet", watch->what, watch->runtime_dir);
    watch->complained = true;
}

static void
on_runtime_dir_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
                       gpointer data)
{
    struct wayland_watch *watch = data;
    g_autofree char *name = g_file_get_basename(file);

    if (!is_socket_name(name))
        return;

    /* A new compositor may have taken a name that an old one left. */
    if (event == G_FILE_MONITOR_EVENT_DELETED || event == G_FILE_MONITOR_EVENT_CREATED)
        g_hash_table_remove(watch->rejected, name);
    if (event == G_FILE_MONITOR_EVENT_CREATED)
        scan(watch);
}

struct wayland_watch *
wayland_watch_new(const char *what, const struct wayland_watch_handler *handler, void *data)
{
    struct wayland_watch *watch = g_new0(struct wayland_watch, 1);
    g_autoptr(GFile) dir = NULL;
    g_autoptr(GError) error = NULL;

    watch->what = g_strdup(what);
    watch->handler = handler;
    watch->data = data;
    watch->runtime_dir = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    watch->rejected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    if (!watch->runtime_dir) {
        g_warning("%s: no XDG_RUNTIME_DIR, and so no Wayland compositor", what);
        return watch;
    }

    dir = g_file_new_for_path(watch->runtime_dir);
    watch->monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_NONE, NULL, &error);
    if (watch->monitor)
        g_signal_connect(watch->monitor, "changed", G_CALLBACK(on_runtime_dir_changed), watch);
    else
        g_warning("%s: cannot watch %s for Wayland compositors: %s", what, watch->runtime_dir,
                  error->message);

    scan(watch);
    return watch;
}

void
wayland_watch_free(struct wayland_watch *watch)
{
    if (!watch)
        return;

    if (watch->monitor)
        g_signal_handlers_disconnect_by_data(watch->monitor, watch);
    g_clear_object(&watch->monitor);
    g_clear_handle_id(&watch->retry_id, g_source_remove);
    disconnect(watch);
    g_hash_table_unref(watch->rejected);
    g_free(watch->runtime_dir);
    g_free(watch->what);
    g_free(watch);
}

struct wl_display *
wayland_watch_display(struct wayland_watch *watch)
{
    return watch->display;
}

const char *
wayland_watch_socket(struct wayland_watch *watch)
{
    return watch->socket;
}

void
wayland_watch_flush(struct wayland_watch *watch)
{
    /* A full socket is sent with the next one; a broken one shows up as a
     * hangup in on_display. */
    if (watch->display)
        wl_display_flush(watch->display);
}

void
wayland_watch_drop(struct wayland_watch *watch, bool reject)
{
    if (reject && watch->socket)
        g_hash_table_add(watch->rejected, g_strdup(watch->socket));
    watch->drop = true;
}
