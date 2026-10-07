/*
 * See selection-backends.h. This is the mutter side of it.
 *
 * mutter gives a remote desktop client the clipboard through a session object
 * on the session bus: CreateSession, then EnableClipboard on what it returns.
 * The session is never started -- it has no screen cast and no input devices,
 * and it is starting one that makes gnome-shell show that the screen is being
 * shared. Data moves through pipes whose descriptors come with the method
 * replies.
 *
 * mutter may not be there yet when we start, goes away when the session ends,
 * and closes its remote desktop sessions when it sees fit, so the session
 * object is set up again whenever it is lost.
 */

#define G_LOG_DOMAIN "clipboard"

#include "selection-backends.h"

#include <stdbool.h>

#include <gio/gunixfdlist.h>
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>

#define MUTTER_NAME "org.gnome.Mutter.RemoteDesktop"
#define MUTTER_PATH "/org/gnome/Mutter/RemoteDesktop"
#define MUTTER_INTERFACE "org.gnome.Mutter.RemoteDesktop"
#define SESSION_INTERFACE "org.gnome.Mutter.RemoteDesktop.Session"

/* How long to leave mutter alone after it refused or closed a session. */
#define RETRY_SECONDS 3

struct mutter_selection {
    /* Cancelled when the selection is freed. A callback that finds its
     * operation cancelled must not touch the selection. */
    GCancellable *cancellable;

    GDBusConnection *connection;
    guint watch_id;
    bool name_owned;

    /* The session object, and whether its clipboard is still being enabled. */
    GDBusProxy *session;
    bool connecting;
    guint retry_id;
    /* Said once that there is no clipboard, and why. */
    bool complained;

    /* What the session's clipboard holds that is not ours. Never NULL. */
    char **mime_types;
    /* What we offer; NULL for nothing. Kept for a session set up later. */
    char **offer;
    /* We have just given the clipboard up, and mutter is about to say that
     * it has no owner. */
    bool withdrawn;

    const struct selection_listener *listener;
    void *listener_data;
};

static void create_session(struct mutter_selection *selection);

static const char *
error_message(GError *error)
{
    g_dbus_error_strip_remote_error(error);
    return error->message;
}

static bool
session_usable(struct mutter_selection *selection)
{
    return selection->session && !selection->connecting;
}

static void
set_mime_types(struct mutter_selection *selection, char **mime_types, bool notify)
{
    g_strfreev(selection->mime_types);
    selection->mime_types = mime_types ? mime_types : g_new0(char *, 1);

    /* Not while the clipboard is being enabled, which is when mutter says
     * what it holds: it cannot be read before that call has returned. */
    if (notify && selection->listener && !selection->connecting)
        selection->listener->owner_changed(selection->listener_data,
                                           (const char *const *)selection->mime_types);
}

static GVariant *
offer_options(char **offer)
{
    GVariantBuilder builder;

    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    /* Without the key the selection is given up. */
    if (offer && offer[0])
        g_variant_builder_add(&builder, "{sv}", "mime-types",
                              g_variant_new_strv((const char *const *)offer, -1));
    return g_variant_new("(a{sv})", &builder);
}

static void
send_offer(struct mutter_selection *selection)
{
    g_dbus_proxy_call(selection->session, "SetSelection", offer_options(selection->offer),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static gboolean
on_retry(gpointer data)
{
    struct mutter_selection *selection = data;

    selection->retry_id = 0;
    create_session(selection);
    return G_SOURCE_REMOVE;
}

/* The session object is no use any more, or never came to be. */
static void
drop_session(struct mutter_selection *selection, bool retry)
{
    bool had_types = selection->mime_types[0] != NULL;

    if (selection->session)
        g_signal_handlers_disconnect_by_data(selection->session, selection);
    g_clear_object(&selection->session);
    selection->connecting = false;

    /* Nothing of the session's can be read without it. */
    set_mime_types(selection, NULL, had_types);

    if (retry && !selection->retry_id)
        selection->retry_id = g_timeout_add_seconds(RETRY_SECONDS, on_retry, selection);
}

static void
session_failed(struct mutter_selection *selection, GError *error)
{
    if (!selection->complained)
        g_message("mutter does not share the session's clipboard (%s); asking again every %d s",
                  error_message(error), RETRY_SECONDS);
    selection->complained = true;
    drop_session(selection, true);
}

/* ------------------------------------------------------------------ */
/* What the session tells us                                           */
/* ------------------------------------------------------------------ */

static void
handle_owner_changed(struct mutter_selection *selection, GVariant *parameters)
{
    g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 0);
    g_autoptr(GVariant) types = g_variant_lookup_value(options, "mime-types", NULL);
    gboolean is_ours = FALSE;
    char **mime_types = NULL;

    g_variant_lookup(options, "session-is-owner", "b", &is_ours);

    /* mutter 50 wraps the list in a tuple; later versions do not. */
    if (types && g_variant_is_of_type(types, G_VARIANT_TYPE("(as)"))) {
        GVariant *list = g_variant_get_child_value(types, 0);

        g_variant_unref(types);
        types = list;
    }
    if (types && g_variant_is_of_type(types, G_VARIANT_TYPE_STRING_ARRAY) && !is_ours)
        mime_types = g_variant_dup_strv(types, NULL);

    /* Neither what we offered ourselves nor that we stopped offering it is
     * news to the listener. It would pass an empty clipboard on to the
     * client, which may be why we gave ours up: because the client has
     * something on its own that does not cross. */
    if (selection->withdrawn && !types)
        is_ours = TRUE;
    selection->withdrawn = false;

    set_mime_types(selection, mime_types, !is_ours);
}

static void
on_session_signal(GDBusProxy *session, const char *sender, const char *signal,
                  GVariant *parameters, gpointer data)
{
    struct mutter_selection *selection = data;

    if (!g_strcmp0(signal, "SelectionOwnerChanged") &&
        g_variant_is_of_type(parameters, G_VARIANT_TYPE("(a{sv})"))) {
        handle_owner_changed(selection, parameters);
    } else if (!g_strcmp0(signal, "SelectionTransfer") &&
               g_variant_is_of_type(parameters, G_VARIANT_TYPE("(su)"))) {
        const char *mime_type;
        guint32 serial;

        g_variant_get(parameters, "(&su)", &mime_type, &serial);
        if (selection->listener)
            selection->listener->requested(selection->listener_data, mime_type, serial);
        else
            mutter_selection_reply(selection, serial, NULL);
    } else if (!g_strcmp0(signal, "Closed")) {
        g_message("mutter closed the clipboard's session; asking for another");
        drop_session(selection, true);
    }
}

/* ------------------------------------------------------------------ */
/* Setting the session up                                              */
/* ------------------------------------------------------------------ */

static void
on_clipboard_enabled(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    struct mutter_selection *selection = data;

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    /* Dropped while the call was on its way. */
    if (selection->session != G_DBUS_PROXY(source))
        return;
    if (!reply) {
        session_failed(selection, error);
        return;
    }

    selection->connecting = false;
    selection->complained = false;
    g_message("sharing the session's clipboard");

    /* Ours replaces what was there; otherwise that is news now. */
    if (selection->offer)
        send_offer(selection);
    else if (selection->mime_types[0] && selection->listener)
        selection->listener->owner_changed(selection->listener_data,
                                           (const char *const *)selection->mime_types);
}

static void
on_session_proxy(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GError) error = NULL;
    GDBusProxy *session = g_dbus_proxy_new_finish(result, &error);
    struct mutter_selection *selection = data;

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    if (!session) {
        session_failed(selection, error);
        return;
    }
    /* mutter went and came back meanwhile, and this is the older attempt. */
    if (selection->session) {
        g_object_unref(session);
        return;
    }

    /* From here on: mutter says what the clipboard holds as part of enabling
     * it, before the call returns. */
    selection->session = session;
    g_signal_connect(session, "g-signal", G_CALLBACK(on_session_signal), selection);

    g_dbus_proxy_call(session, "EnableClipboard", offer_options(NULL), G_DBUS_CALL_FLAGS_NONE, -1,
                      selection->cancellable, on_clipboard_enabled, selection);
}

static void
on_session_created(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply =
        g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    struct mutter_selection *selection = data;
    const char *path;

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    if (!reply) {
        session_failed(selection, error);
        return;
    }

    g_variant_get(reply, "(&o)", &path);
    g_dbus_proxy_new(selection->connection,
                     G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                         G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                     NULL, MUTTER_NAME, path, SESSION_INTERFACE, selection->cancellable,
                     on_session_proxy, selection);
}

static void
create_session(struct mutter_selection *selection)
{
    if (!selection->name_owned || selection->session || selection->connecting)
        return;

    selection->connecting = true;
    g_dbus_connection_call(selection->connection, MUTTER_NAME, MUTTER_PATH, MUTTER_INTERFACE,
                           "CreateSession", NULL, G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE,
                           -1, selection->cancellable, on_session_created, selection);
}

static void
on_name_appeared(GDBusConnection *connection, const char *name, const char *owner, gpointer data)
{
    struct mutter_selection *selection = data;

    selection->name_owned = true;
    create_session(selection);
}

static void
on_name_vanished(GDBusConnection *connection, const char *name, gpointer data)
{
    struct mutter_selection *selection = data;

    selection->name_owned = false;
    g_clear_handle_id(&selection->retry_id, g_source_remove);
    drop_session(selection, false);

    if (!selection->complained)
        g_message("no %s on the session bus; no clipboard until there is", MUTTER_NAME);
    selection->complained = true;
}

static void
on_bus(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GError) error = NULL;
    GDBusConnection *connection = g_bus_get_finish(result, &error);
    struct mutter_selection *selection = data;

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    if (!connection) {
        g_warning("no session bus, and so no clipboard: %s", error->message);
        return;
    }

    /* Ours to outlive: weaselwayd is not to exit because the bus went. */
    g_dbus_connection_set_exit_on_close(connection, FALSE);
    selection->connection = connection;
    selection->watch_id =
        g_bus_watch_name_on_connection(connection, MUTTER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                       on_name_appeared, on_name_vanished, selection, NULL);
}

struct mutter_selection *
mutter_selection_new(void)
{
    struct mutter_selection *selection = g_new0(struct mutter_selection, 1);

    selection->cancellable = g_cancellable_new();
    selection->mime_types = g_new0(char *, 1);
    g_bus_get(G_BUS_TYPE_SESSION, selection->cancellable, on_bus, selection);
    return selection;
}

void
mutter_selection_free(struct mutter_selection *selection)
{
    if (!selection)
        return;

    g_cancellable_cancel(selection->cancellable);
    g_clear_handle_id(&selection->watch_id, g_bus_unwatch_name);
    g_clear_handle_id(&selection->retry_id, g_source_remove);
    if (selection->session)
        g_signal_handlers_disconnect_by_data(selection->session, selection);

    /* mutter closes the session when we leave the bus. */
    g_clear_object(&selection->session);
    g_clear_object(&selection->connection);
    g_clear_object(&selection->cancellable);
    g_strfreev(selection->mime_types);
    g_strfreev(selection->offer);
    g_free(selection);
}

void
mutter_selection_set_listener(struct mutter_selection *selection,
                              const struct selection_listener *listener, void *data)
{
    selection->listener = listener;
    selection->listener_data = data;
}

const char *const *
mutter_selection_get_mime_types(struct mutter_selection *selection)
{
    return (const char *const *)selection->mime_types;
}

void
mutter_selection_offer(struct mutter_selection *selection, const char *const *mime_types)
{
    bool had_offer = selection->offer != NULL;

    g_strfreev(selection->offer);
    selection->offer = mime_types && mime_types[0] ? g_strdupv((char **)mime_types) : NULL;

    /* Nothing to give up if nothing was taken. */
    if (session_usable(selection) && (selection->offer || had_offer)) {
        selection->withdrawn = !selection->offer;
        send_offer(selection);
    }
}

/* ------------------------------------------------------------------ */
/* Handing data to the session                                         */
/* ------------------------------------------------------------------ */

/* The pipe that came with the reply to SelectionWrite or SelectionRead, set
 * not to block: the other end takes its time, and the main loop must not wait
 * for it. -1 with @error if there is none. */
static int
take_pipe(GVariant *ret, GUnixFDList *fds, GError **error)
{
    g_autofd int fd = -1;
    gint32 handle;

    g_variant_get(ret, "(h)", &handle);
    if (!fds) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "no descriptor in the reply");
        return -1;
    }

    fd = g_unix_fd_list_get(fds, handle, error);
    if (fd < 0 || !g_unix_set_fd_nonblocking(fd, TRUE, error))
        return -1;
    return g_steal_fd(&fd);
}

struct reply {
    GDBusProxy *session;
    uint32_t serial;
    GBytes *data;
    GOutputStream *stream;
};

/* Tell mutter how it went, and be done with it. */
static void
reply_finish(struct reply *reply, bool success)
{
    g_dbus_proxy_call(reply->session, "SelectionWriteDone",
                      g_variant_new("(ub)", reply->serial, success), G_DBUS_CALL_FLAGS_NONE, -1,
                      NULL, NULL, NULL);

    g_clear_object(&reply->stream);
    g_clear_pointer(&reply->data, g_bytes_unref);
    g_object_unref(reply->session);
    g_free(reply);
}

static void
on_reply_written(GObject *source, GAsyncResult *result, gpointer data)
{
    struct reply *reply = data;
    g_autoptr(GError) error = NULL;
    bool written = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, &error);

    /* A reader that had enough closes its end; nothing to report. */
    if (!written && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
        g_warning("writing clipboard data to the session failed: %s", error->message);

    /* The end of the pipe is what tells the reader that this is all. */
    g_output_stream_close(reply->stream, NULL, NULL);
    reply_finish(reply, written);
}

static void
on_reply_fd(GObject *source, GAsyncResult *result, gpointer data)
{
    struct reply *reply = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GUnixFDList) fds = NULL;
    g_autoptr(GVariant) ret =
        g_dbus_proxy_call_with_unix_fd_list_finish(G_DBUS_PROXY(source), &fds, result, &error);
    int fd;

    if (!ret) {
        /* mutter gave up on the request before we had the data. */
        g_debug("the session no longer wants request %u: %s", reply->serial,
                error_message(error));
        reply_finish(reply, false);
        return;
    }

    fd = take_pipe(ret, fds, &error);
    if (fd < 0) {
        g_warning("no pipe for request %u: %s", reply->serial, error->message);
        reply_finish(reply, false);
        return;
    }

    reply->stream = g_unix_output_stream_new(fd, TRUE);
    g_output_stream_write_all_async(reply->stream, g_bytes_get_data(reply->data, NULL),
                                    g_bytes_get_size(reply->data), G_PRIORITY_DEFAULT, NULL,
                                    on_reply_written, reply);
}

void
mutter_selection_reply(struct mutter_selection *selection, uint32_t serial, GBytes *data)
{
    struct reply *reply;

    if (!selection->session)
        return;

    reply = g_new0(struct reply, 1);
    reply->session = g_object_ref(selection->session);
    reply->serial = serial;

    if (!data) {
        reply_finish(reply, false);
        return;
    }

    reply->data = g_bytes_ref(data);
    g_dbus_proxy_call_with_unix_fd_list(reply->session, "SelectionWrite",
                                        g_variant_new("(u)", serial), G_DBUS_CALL_FLAGS_NONE, -1,
                                        NULL, NULL, on_reply_fd, reply);
}

/* ------------------------------------------------------------------ */
/* Reading from the session                                            */
/* ------------------------------------------------------------------ */

static void
on_read_fd(GObject *source, GAsyncResult *result, gpointer data)
{
    GTask *task = data;
    GError *error = NULL;
    g_autoptr(GUnixFDList) fds = NULL;
    g_autoptr(GVariant) ret =
        g_dbus_proxy_call_with_unix_fd_list_finish(G_DBUS_PROXY(source), &fds, result, &error);
    int fd;

    if (!ret) {
        g_dbus_error_strip_remote_error(error);
        g_task_return_error(task, error);
        g_object_unref(task);
        return;
    }

    fd = take_pipe(ret, fds, &error);
    if (fd < 0) {
        g_task_return_error(task, error);
        g_object_unref(task);
        return;
    }

    selection_read_pipe(task, fd);
}

void
mutter_selection_read(struct mutter_selection *selection, const char *mime_type, GTask *task)
{
    if (!session_usable(selection)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                                "the session's clipboard is not available");
        g_object_unref(task);
        return;
    }

    g_dbus_proxy_call_with_unix_fd_list(selection->session, "SelectionRead",
                                        g_variant_new("(s)", mime_type), G_DBUS_CALL_FLAGS_NONE,
                                        -1, NULL, g_task_get_cancellable(task), on_read_fd, task);
}
