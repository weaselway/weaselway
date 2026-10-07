/*
 * See selection.h. Both backends of selection-backends.h run side by side:
 * mutter never has ext-data-control-v1, and nothing else has mutter's D-Bus
 * interface, so at most one of them finds a compositor. What we offer goes to
 * both, so that it is there whichever one does.
 */

#define G_LOG_DOMAIN "clipboard"

#include "selection-backends.h"

#include <gio/gunixinputstream.h>

enum backend { MUTTER, WAYLAND };

struct request {
    enum backend backend;
    uint32_t serial;
};

struct selection {
    struct mutter_selection *mutter;
    struct wayland_selection *wayland;

    /* Our serials for the backends' requests, which each count on their own. */
    GHashTable *requests;
    uint32_t next_serial;

    const struct selection_listener *listener;
    void *listener_data;
};

static const char *const no_mime_types[] = { NULL };

static void
backend_reply(struct selection *selection, enum backend backend, uint32_t serial, GBytes *data)
{
    if (backend == MUTTER)
        mutter_selection_reply(selection->mutter, serial, data);
    else
        wayland_selection_reply(selection->wayland, serial, data);
}

static void
owner_changed(struct selection *selection, const char *const *mime_types)
{
    if (selection->listener)
        selection->listener->owner_changed(selection->listener_data, mime_types);
}

static void
requested(struct selection *selection, enum backend backend, const char *mime_type,
          uint32_t backend_serial)
{
    struct request *request;
    uint32_t serial;

    if (!selection->listener) {
        backend_reply(selection, backend, backend_serial, NULL);
        return;
    }

    do
        serial = ++selection->next_serial;
    while (!serial || g_hash_table_contains(selection->requests, GUINT_TO_POINTER(serial)));

    request = g_new(struct request, 1);
    request->backend = backend;
    request->serial = backend_serial;
    g_hash_table_insert(selection->requests, GUINT_TO_POINTER(serial), request);
    selection->listener->requested(selection->listener_data, mime_type, serial);
}

static void
on_mutter_owner_changed(void *data, const char *const *mime_types)
{
    owner_changed(data, mime_types);
}

static void
on_mutter_requested(void *data, const char *mime_type, uint32_t serial)
{
    requested(data, MUTTER, mime_type, serial);
}

static void
on_wayland_owner_changed(void *data, const char *const *mime_types)
{
    owner_changed(data, mime_types);
}

static void
on_wayland_requested(void *data, const char *mime_type, uint32_t serial)
{
    requested(data, WAYLAND, mime_type, serial);
}

static const struct selection_listener mutter_listener = {
    on_mutter_owner_changed,
    on_mutter_requested,
};

static const struct selection_listener wayland_listener = {
    on_wayland_owner_changed,
    on_wayland_requested,
};

struct selection *
selection_new(void)
{
    struct selection *selection = g_new0(struct selection, 1);

    selection->requests = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    selection->mutter = mutter_selection_new();
    selection->wayland = wayland_selection_new();
    mutter_selection_set_listener(selection->mutter, &mutter_listener, selection);
    wayland_selection_set_listener(selection->wayland, &wayland_listener, selection);
    return selection;
}

void
selection_free(struct selection *selection)
{
    if (!selection)
        return;

    mutter_selection_free(selection->mutter);
    wayland_selection_free(selection->wayland);
    g_hash_table_unref(selection->requests);
    g_free(selection);
}

void
selection_set_listener(struct selection *selection, const struct selection_listener *listener,
                       void *data)
{
    selection->listener = listener;
    selection->listener_data = data;
}

const char *const *
selection_get_mime_types(struct selection *selection)
{
    const char *const *mime_types = mutter_selection_get_mime_types(selection->mutter);

    if (mime_types[0])
        return mime_types;
    mime_types = wayland_selection_get_mime_types(selection->wayland);
    return mime_types[0] ? mime_types : no_mime_types;
}

void
selection_offer(struct selection *selection, const char *const *mime_types)
{
    mutter_selection_offer(selection->mutter, mime_types);
    wayland_selection_offer(selection->wayland, mime_types);
}

void
selection_reply(struct selection *selection, uint32_t serial, GBytes *data)
{
    struct request *request = g_hash_table_lookup(selection->requests, GUINT_TO_POINTER(serial));

    if (!request)
        return;
    backend_reply(selection, request->backend, request->serial, data);
    g_hash_table_remove(selection->requests, GUINT_TO_POINTER(serial));
}

void
selection_read_async(struct selection *selection, const char *mime_type,
                     GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);

    g_task_set_source_tag(task, selection_read_async);

    /* From whichever has something; mutter's says why if neither has. */
    if (wayland_selection_get_mime_types(selection->wayland)[0] &&
        !mutter_selection_get_mime_types(selection->mutter)[0])
        wayland_selection_read(selection->wayland, mime_type, task);
    else
        mutter_selection_read(selection->mutter, mime_type, task);
}

GBytes *
selection_read_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* More than this is not read from the clipboard's owner, which decides how
 * much it writes: an 8K screenshot as a bitmap is half of it. A power of two,
 * as the stream's sizes are. */
#define MAX_READ_SIZE ((gsize)256 << 20)

/* Lets the stream that collects a read grow no further than that; the write
 * that would then fails with G_IO_ERROR_NO_SPACE. */
static gpointer
realloc_limited(gpointer data, gsize size)
{
    return size <= MAX_READ_SIZE ? g_try_realloc(data, size) : NULL;
}

static void
on_read_spliced(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GTask) task = data;
    GError *error = NULL;

    if (g_output_stream_splice_finish(G_OUTPUT_STREAM(source), result, &error) < 0)
        g_task_return_error(task, error);
    else
        g_task_return_pointer(task,
                              g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(source)),
                              (GDestroyNotify)g_bytes_unref);
}

void
selection_read_pipe(GTask *task, int fd)
{
    g_autoptr(GInputStream) in = g_unix_input_stream_new(fd, TRUE);
    g_autoptr(GOutputStream) out = g_memory_output_stream_new(NULL, 0, realloc_limited, g_free);

    /* Until the owner closes its end of the pipe. */
    g_output_stream_splice_async(out, in,
                                 G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                     G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                 G_PRIORITY_DEFAULT, g_task_get_cancellable(task),
                                 on_read_spliced, task);
}
