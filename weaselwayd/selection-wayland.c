/*
 * See selection-backends.h. This is the ext-data-control-v1 side of it, for
 * KWin and the wlroots compositors.
 *
 * The protocol gives a client without a window the seat's clipboard. The
 * compositor starts after weaselwayd and may come and go, so the runtime
 * directory is watched for Wayland sockets. A compositor without the protocol
 * (mutter) is left alone until its socket is replaced.
 *
 * Whether the clipboard is ours: KWin cancels the old source before it
 * announces the new selection, so while our source has not been cancelled,
 * a selection event is about our own.
 */

#define G_LOG_DOMAIN "clipboard"

#include "selection-backends.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"

/* How long to wait for a socket that refused the connection. */
#define RETRY_SECONDS 3

struct wayland_selection {
    char *runtime_dir;
    GFileMonitor *monitor;
    guint retry_id;
    /* Sockets whose compositor does not have the protocol, until they go. */
    GHashTable *rejected;
    /* Said once that there is no clipboard here. */
    bool complained;

    /* The compositor, once a socket answered; ready once there is a device. */
    char *socket;
    struct wl_display *display;
    guint display_id;
    struct wl_registry *registry;
    struct wl_callback *sync;
    struct wl_seat *seat;
    struct ext_data_control_manager_v1 *manager;
    struct ext_data_control_device_v1 *device;
    /* A handler found the connection to be of no use; dropped after dispatch. */
    bool drop;

    /* The selection when it is not ours, and what it holds. Never NULL. */
    struct ext_data_control_offer_v1 *offer;
    char **mime_types;

    /* What we offer; NULL for nothing. Kept for a compositor that comes later. */
    char **own;
    /* What offers it, until the compositor cancels it. */
    struct ext_data_control_source_v1 *source;
    /* We have just given the clipboard up; the empty selection is ours. */
    bool withdrawn;

    /* Our serial -> the pipe to answer it on. */
    GHashTable *requests;
    uint32_t next_serial;

    const struct selection_listener *listener;
    void *listener_data;
};

static void scan(struct wayland_selection *selection);

static void
flush(struct wayland_selection *selection)
{
    /* A full socket is sent with the next one; a broken one shows up as a
     * hangup in on_display. */
    if (selection->display)
        wl_display_flush(selection->display);
}

static void
set_mime_types(struct wayland_selection *selection, char **mime_types, bool notify)
{
    g_strfreev(selection->mime_types);
    selection->mime_types = mime_types ? mime_types : g_new0(char *, 1);

    if (notify && selection->listener)
        selection->listener->owner_changed(selection->listener_data,
                                           (const char *const *)selection->mime_types);
}

/* ------------------------------------------------------------------ */
/* What the compositor offers                                          */
/* ------------------------------------------------------------------ */

static void
destroy_offer(struct ext_data_control_offer_v1 *offer)
{
    if (!offer)
        return;
    g_ptr_array_unref(ext_data_control_offer_v1_get_user_data(offer));
    ext_data_control_offer_v1_destroy(offer);
}

static void
on_offer_mime_type(void *data, struct ext_data_control_offer_v1 *offer, const char *mime_type)
{
    g_ptr_array_add(data, g_strdup(mime_type));
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
    .offer = on_offer_mime_type,
};

static void
on_data_offer(void *data, struct ext_data_control_device_v1 *device,
              struct ext_data_control_offer_v1 *offer)
{
    ext_data_control_offer_v1_add_listener(offer, &offer_listener,
                                           g_ptr_array_new_with_free_func(g_free));
}

static void
on_selection(void *data, struct ext_data_control_device_v1 *device,
             struct ext_data_control_offer_v1 *offer)
{
    struct wayland_selection *selection = data;
    bool withdrawn = selection->withdrawn && !offer;
    GPtrArray *types;
    char **mime_types;

    selection->withdrawn = false;
    destroy_offer(selection->offer);
    selection->offer = NULL;

    if (offer && selection->source) {
        destroy_offer(offer);
        set_mime_types(selection, NULL, false);
        return;
    }
    if (!offer) {
        set_mime_types(selection, NULL, !withdrawn);
        return;
    }

    selection->offer = offer;
    types = ext_data_control_offer_v1_get_user_data(offer);
    mime_types = g_new0(char *, types->len + 1);
    for (guint i = 0; i < types->len; i++)
        mime_types[i] = g_strdup(g_ptr_array_index(types, i));
    set_mime_types(selection, mime_types, true);
}

static void
on_primary_selection(void *data, struct ext_data_control_device_v1 *device,
                     struct ext_data_control_offer_v1 *offer)
{
    destroy_offer(offer);
}

static void
on_finished(void *data, struct ext_data_control_device_v1 *device)
{
    struct wayland_selection *selection = data;

    g_message("%s took the clipboard away; looking for it again", selection->socket);
    selection->drop = true;
}

static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = on_data_offer,
    .selection = on_selection,
    .finished = on_finished,
    .primary_selection = on_primary_selection,
};

/* ------------------------------------------------------------------ */
/* What we offer                                                       */
/* ------------------------------------------------------------------ */

static void
on_source_send(void *data, struct ext_data_control_source_v1 *source, const char *mime_type,
               int32_t fd)
{
    struct wayland_selection *selection = data;
    uint32_t serial;

    if (!selection->listener) {
        close(fd);
        return;
    }

    do
        serial = ++selection->next_serial;
    while (!serial || g_hash_table_contains(selection->requests, GUINT_TO_POINTER(serial)));
    g_hash_table_insert(selection->requests, GUINT_TO_POINTER(serial), GINT_TO_POINTER(fd));
    selection->listener->requested(selection->listener_data, mime_type, serial);
}

static void
on_source_cancelled(void *data, struct ext_data_control_source_v1 *source)
{
    struct wayland_selection *selection = data;

    /* Someone else's now; what we had is gone with it. */
    if (source == selection->source) {
        selection->source = NULL;
        g_clear_pointer(&selection->own, g_strfreev);
    }
    ext_data_control_source_v1_destroy(source);
}

static const struct ext_data_control_source_v1_listener source_listener = {
    .send = on_source_send,
    .cancelled = on_source_cancelled,
};

/* Make the clipboard what selection->own says. */
static void
take(struct wayland_selection *selection)
{
    struct ext_data_control_source_v1 *source = NULL;

    if (selection->own) {
        source = ext_data_control_manager_v1_create_data_source(selection->manager);
        ext_data_control_source_v1_add_listener(source, &source_listener, selection);
        for (char **type = selection->own; *type; type++)
            ext_data_control_source_v1_offer(source, *type);
    }

    ext_data_control_device_v1_set_selection(selection->device, source);
    /* The compositor cancels the one it replaces; this is just ours no more. */
    if (selection->source)
        ext_data_control_source_v1_destroy(selection->source);
    selection->source = source;
    selection->withdrawn = !source;
}

/* ------------------------------------------------------------------ */
/* The connection                                                      */
/* ------------------------------------------------------------------ */

static void
disconnect(struct wayland_selection *selection)
{
    bool had_types = selection->mime_types[0] != NULL;

    g_clear_handle_id(&selection->display_id, g_source_remove);
    destroy_offer(selection->offer);
    selection->offer = NULL;
    g_clear_pointer(&selection->source, ext_data_control_source_v1_destroy);
    g_clear_pointer(&selection->device, ext_data_control_device_v1_destroy);
    g_clear_pointer(&selection->manager, ext_data_control_manager_v1_destroy);
    g_clear_pointer(&selection->seat, wl_seat_destroy);
    g_clear_pointer(&selection->sync, wl_callback_destroy);
    g_clear_pointer(&selection->registry, wl_registry_destroy);
    g_clear_pointer(&selection->display, wl_display_disconnect);
    g_clear_pointer(&selection->socket, g_free);
    selection->withdrawn = false;
    selection->drop = false;

    /* Nothing of the session's can be read without it. */
    set_mime_types(selection, NULL, had_types);
}

static void
on_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
          uint32_t version)
{
    struct wayland_selection *selection = data;

    if (!strcmp(interface, wl_seat_interface.name) && !selection->seat)
        selection->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (!strcmp(interface, ext_data_control_manager_v1_interface.name) && !selection->manager)
        selection->manager =
            wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1);
}

static void
on_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    /* A seat that goes finishes the device. */
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

/* The compositor has told us its globals. */
static void
on_sync(void *data, struct wl_callback *callback, uint32_t time)
{
    struct wayland_selection *selection = data;

    g_clear_pointer(&selection->sync, wl_callback_destroy);

    if (!selection->manager || !selection->seat) {
        g_debug("%s has no ext-data-control-v1", selection->socket);
        g_hash_table_add(selection->rejected, g_strdup(selection->socket));
        selection->drop = true;
        return;
    }

    selection->device =
        ext_data_control_manager_v1_get_data_device(selection->manager, selection->seat);
    ext_data_control_device_v1_add_listener(selection->device, &device_listener, selection);
    selection->complained = false;
    g_message("sharing the session's clipboard (ext-data-control-v1 on %s)", selection->socket);

    /* Ours replaces what is there; otherwise the compositor says what that is. */
    if (selection->own)
        take(selection);
}

static const struct wl_callback_listener sync_listener = {
    .done = on_sync,
};

static gboolean
on_display(int fd, GIOCondition condition, gpointer data)
{
    struct wayland_selection *selection = data;

    if (!(condition & G_IO_IN) || wl_display_dispatch(selection->display) < 0) {
        g_message("lost the connection to %s", selection->socket);
        selection->drop = true;
    }

    if (selection->drop) {
        /* The source is gone with the return value. */
        selection->display_id = 0;
        disconnect(selection);
        scan(selection);
        return G_SOURCE_REMOVE;
    }

    flush(selection);
    return G_SOURCE_CONTINUE;
}

static gboolean
on_retry(gpointer data)
{
    struct wayland_selection *selection = data;

    selection->retry_id = 0;
    scan(selection);
    return G_SOURCE_REMOVE;
}

/* Connect to @name and ask for its globals. False if nobody answers. */
static bool
try_socket(struct wayland_selection *selection, const char *name)
{
    g_autofree char *path = g_build_filename(selection->runtime_dir, name, NULL);

    selection->display = wl_display_connect(path);
    if (!selection->display) {
        g_debug("cannot connect to %s: %s", name, g_strerror(errno));
        return false;
    }

    selection->socket = g_strdup(name);
    selection->registry = wl_display_get_registry(selection->display);
    wl_registry_add_listener(selection->registry, &registry_listener, selection);
    selection->sync = wl_display_sync(selection->display);
    wl_callback_add_listener(selection->sync, &sync_listener, selection);
    selection->display_id =
        g_unix_fd_add(wl_display_get_fd(selection->display), G_IO_IN | G_IO_HUP | G_IO_ERR,
                      on_display, selection);
    flush(selection);
    return true;
}

static bool
is_socket_name(const char *name)
{
    return g_str_has_prefix(name, "wayland-") && !g_str_has_suffix(name, ".lock");
}

/* Look for a compositor, unless there is one. */
static void
scan(struct wayland_selection *selection)
{
    g_autoptr(GDir) dir = NULL;
    bool refused = false;
    const char *name;

    if (selection->display)
        return;

    dir = g_dir_open(selection->runtime_dir, 0, NULL);
    while (dir && (name = g_dir_read_name(dir))) {
        if (!is_socket_name(name) || g_hash_table_contains(selection->rejected, name))
            continue;
        if (try_socket(selection, name))
            return;
        refused = true;
    }

    /* Bound, but not yet listening. */
    if (refused && !selection->retry_id)
        selection->retry_id = g_timeout_add_seconds(RETRY_SECONDS, on_retry, selection);

    if (!selection->complained)
        g_debug("no compositor with ext-data-control-v1 in %s yet", selection->runtime_dir);
    selection->complained = true;
}

static void
on_runtime_dir_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
                       gpointer data)
{
    struct wayland_selection *selection = data;
    g_autofree char *name = g_file_get_basename(file);

    if (!is_socket_name(name))
        return;

    /* A new compositor may have taken a name that an old one left. */
    if (event == G_FILE_MONITOR_EVENT_DELETED || event == G_FILE_MONITOR_EVENT_CREATED)
        g_hash_table_remove(selection->rejected, name);
    if (event == G_FILE_MONITOR_EVENT_CREATED)
        scan(selection);
}

/* ------------------------------------------------------------------ */
/* selection-backends.h                                                */
/* ------------------------------------------------------------------ */

static void
close_fd(gpointer fd)
{
    close(GPOINTER_TO_INT(fd));
}

struct wayland_selection *
wayland_selection_new(void)
{
    struct wayland_selection *selection = g_new0(struct wayland_selection, 1);
    g_autoptr(GFile) dir = NULL;
    g_autoptr(GError) error = NULL;

    selection->runtime_dir = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    selection->rejected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    selection->requests = g_hash_table_new_full(NULL, NULL, NULL, close_fd);
    selection->mime_types = g_new0(char *, 1);

    if (!selection->runtime_dir) {
        g_warning("no XDG_RUNTIME_DIR, and so no Wayland compositor to share a clipboard with");
        return selection;
    }

    dir = g_file_new_for_path(selection->runtime_dir);
    selection->monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_NONE, NULL, &error);
    if (selection->monitor)
        g_signal_connect(selection->monitor, "changed", G_CALLBACK(on_runtime_dir_changed),
                         selection);
    else
        g_warning("cannot watch %s for Wayland compositors: %s", selection->runtime_dir,
                  error->message);

    scan(selection);
    return selection;
}

void
wayland_selection_free(struct wayland_selection *selection)
{
    if (!selection)
        return;

    selection->listener = NULL;
    if (selection->monitor)
        g_signal_handlers_disconnect_by_data(selection->monitor, selection);
    g_clear_object(&selection->monitor);
    g_clear_handle_id(&selection->retry_id, g_source_remove);
    disconnect(selection);
    g_hash_table_unref(selection->requests);
    g_hash_table_unref(selection->rejected);
    g_strfreev(selection->mime_types);
    g_strfreev(selection->own);
    g_free(selection->runtime_dir);
    g_free(selection);
}

void
wayland_selection_set_listener(struct wayland_selection *selection,
                               const struct selection_listener *listener, void *data)
{
    selection->listener = listener;
    selection->listener_data = data;
}

const char *const *
wayland_selection_get_mime_types(struct wayland_selection *selection)
{
    return (const char *const *)selection->mime_types;
}

void
wayland_selection_offer(struct wayland_selection *selection, const char *const *mime_types)
{
    g_strfreev(selection->own);
    selection->own = mime_types && mime_types[0] ? g_strdupv((char **)mime_types) : NULL;

    /* Giving up what someone else has taken would clear theirs. */
    if (selection->device && (selection->own || selection->source)) {
        take(selection);
        flush(selection);
    }
}

static void
on_reply_written(GObject *source, GAsyncResult *result, gpointer data)
{
    GOutputStream *stream = G_OUTPUT_STREAM(source);
    g_autoptr(GError) error = NULL;

    /* A reader that had enough closes its end; nothing to report. */
    if (!g_output_stream_write_all_finish(stream, result, NULL, &error) &&
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
        g_warning("writing clipboard data to the session failed: %s", error->message);

    /* The end of the pipe is what tells the reader that this is all. */
    g_output_stream_close(stream, NULL, NULL);
    g_object_unref(stream);
    g_bytes_unref(data);
}

void
wayland_selection_reply(struct wayland_selection *selection, uint32_t serial, GBytes *data)
{
    gpointer value;
    GOutputStream *stream;
    int fd;

    if (!g_hash_table_steal_extended(selection->requests, GUINT_TO_POINTER(serial), NULL, &value))
        return;
    fd = GPOINTER_TO_INT(value);

    /* The reader sees nothing but the end. */
    if (!data || !g_unix_set_fd_nonblocking(fd, TRUE, NULL)) {
        close(fd);
        return;
    }

    stream = g_unix_output_stream_new(fd, TRUE);
    g_output_stream_write_all_async(stream, g_bytes_get_data(data, NULL), g_bytes_get_size(data),
                                    G_PRIORITY_DEFAULT, NULL, on_reply_written,
                                    g_bytes_ref(data));
}

void
wayland_selection_read(struct wayland_selection *selection, const char *mime_type, GTask *task)
{
    GError *error = NULL;
    int fds[2];

    if (!selection->offer) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                                "the session's clipboard is not available");
        g_object_unref(task);
        return;
    }

    if (!g_unix_open_pipe(fds, O_CLOEXEC, &error) ||
        !g_unix_set_fd_nonblocking(fds[0], TRUE, &error)) {
        g_task_return_error(task, error);
        g_object_unref(task);
        return;
    }

    /* libwayland sends a copy of the write end; ours goes, or the read would
     * never see the end. */
    ext_data_control_offer_v1_receive(selection->offer, mime_type, fds[1]);
    flush(selection);
    close(fds[1]);
    selection_read_pipe(task, fds[0]);
}
