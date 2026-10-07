/*
 * The session's clipboard, as weaselwayd sees it: what its owner offers, a way
 * to read that, and a way to own it on behalf of the Windows client.
 *
 * There is no Wayland protocol for this that every compositor has. mutter is
 * reached over the D-Bus interface that gnome-remote-desktop uses
 * (org.gnome.Mutter.RemoteDesktop), KWin and the wlroots compositors through
 * ext-data-control-v1; see selection-backends.h. Without a compositor that
 * answers, nothing is offered and nothing can be read, and that changes as
 * soon as one shows up.
 *
 * Everything runs on the thread-default main context of selection_new().
 */

#ifndef WEASELWAY_SELECTION_H
#define WEASELWAY_SELECTION_H

#include <stdint.h>

#include <gio/gio.h>

struct selection;

struct selection_listener {
    /* Something in the session took the clipboard, or gave it up: @mime_types
     * is what can be read now, NULL-terminated and possibly empty. Not called
     * for what selection_offer() put there. */
    void (*owner_changed)(void *data, const char *const *mime_types);
    /* Something in the session is pasting what selection_offer() offered. To
     * be answered with selection_reply() and the same @serial. */
    void (*requested)(void *data, const char *mime_type, uint32_t serial);
};

struct selection *selection_new(void);
void selection_free(struct selection *selection);

/* One listener at a time; NULL for none. */
void selection_set_listener(struct selection *selection,
                            const struct selection_listener *listener, void *data);

/* What the session's clipboard holds that is not ours; NULL-terminated, and
 * never NULL. */
const char *const *selection_get_mime_types(struct selection *selection);

/* Take the clipboard: @mime_types can be had from us. NULL, or an empty list,
 * gives it up again. */
void selection_offer(struct selection *selection, const char *const *mime_types);

/* The answer to selection_listener::requested. NULL @data if there is none
 * to be had. */
void selection_reply(struct selection *selection, uint32_t serial, GBytes *data);

/* Read what the clipboard's owner has as @mime_type. One at a time. */
void selection_read_async(struct selection *selection, const char *mime_type,
                          GCancellable *cancellable, GAsyncReadyCallback callback,
                          gpointer user_data);
GBytes *selection_read_finish(GAsyncResult *result, GError **error);

#endif
