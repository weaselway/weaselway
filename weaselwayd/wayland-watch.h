/*
 * A connection to the session's Wayland compositor, for a part of weaselwayd
 * that needs some of its globals (the clipboard, the screen layout).
 *
 * The compositor starts after weaselwayd and may come and go, so the runtime
 * directory is watched for Wayland sockets. A socket that answers is asked for
 * its globals. A user that does not find what it needs drops the connection
 * and rejects the socket, which is then left alone until it is replaced.
 */

#ifndef WEASELWAY_WAYLAND_WATCH_H
#define WEASELWAY_WAYLAND_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#include <wayland-client.h>

struct wayland_watch;

struct wayland_watch_handler {
    /* A global of the connection, at first or later. */
    void (*global)(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version);
    /* A global went away. May be NULL. */
    void (*global_remove)(void *data, uint32_t name);
    /* The first globals are all in. */
    void (*ready)(void *data);
    /* The connection is going: destroy every proxy made on it. */
    void (*disconnected)(void *data);
};

/* @what names the user in the log. */
struct wayland_watch *wayland_watch_new(const char *what, const struct wayland_watch_handler *handler,
                                        void *data);
void wayland_watch_free(struct wayland_watch *watch);

/* The connection and its socket's name; NULL while there is none. */
struct wl_display *wayland_watch_display(struct wayland_watch *watch);
const char *wayland_watch_socket(struct wayland_watch *watch);

/* Send what is queued. */
void wayland_watch_flush(struct wayland_watch *watch);

/* From a handler: the connection is of no use. It is dropped once the events
 * are dispatched, and another socket is looked for. With @reject, this one is
 * skipped until it is replaced. */
void wayland_watch_drop(struct wayland_watch *watch, bool reject);

#endif
