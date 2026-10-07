/*
 * The two ways selection.c reaches the session's clipboard. Each has the calls
 * of selection.h, with the same meaning; selection.c runs both and uses the
 * one whose compositor answers.
 *
 *   mutter_   mutter's D-Bus interface for remote desktops (selection-mutter.c)
 *   wayland_  ext-data-control-v1, which KWin and the wlroots compositors
 *             implement (selection-wayland.c)
 *
 * A read takes a GTask of selection_read_async() and returns it.
 */

#ifndef WEASELWAY_SELECTION_BACKENDS_H
#define WEASELWAY_SELECTION_BACKENDS_H

#include "selection.h"

struct mutter_selection;

struct mutter_selection *mutter_selection_new(void);
void mutter_selection_free(struct mutter_selection *selection);
void mutter_selection_set_listener(struct mutter_selection *selection,
                                   const struct selection_listener *listener, void *data);
const char *const *mutter_selection_get_mime_types(struct mutter_selection *selection);
void mutter_selection_offer(struct mutter_selection *selection, const char *const *mime_types);
void mutter_selection_reply(struct mutter_selection *selection, uint32_t serial, GBytes *data);
void mutter_selection_read(struct mutter_selection *selection, const char *mime_type,
                           GTask *task);

struct wayland_selection;

struct wayland_selection *wayland_selection_new(void);
void wayland_selection_free(struct wayland_selection *selection);
void wayland_selection_set_listener(struct wayland_selection *selection,
                                    const struct selection_listener *listener, void *data);
const char *const *wayland_selection_get_mime_types(struct wayland_selection *selection);
void wayland_selection_offer(struct wayland_selection *selection, const char *const *mime_types);
void wayland_selection_reply(struct wayland_selection *selection, uint32_t serial, GBytes *data);
void wayland_selection_read(struct wayland_selection *selection, const char *mime_type,
                            GTask *task);

/* Returns @task with what comes out of @fd, a non-blocking pipe, until the
 * other end closes it. Takes both. */
void selection_read_pipe(GTask *task, int fd);

#endif
