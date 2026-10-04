/*
 * weaselwayd's clipboard: what is copied in the session can be pasted on
 * Windows and the other way around. This is the RDP end, the cliprdr channel
 * of one client; the session's end is selection.h.
 *
 * Text, HTML and images cross. Files do not.
 */

#ifndef WEASELWAY_CLIPBOARD_H
#define WEASELWAY_CLIPBOARD_H

#include <freerdp/freerdp.h>
#include <winpr/wtypes.h>

struct clipboard;
struct selection;

/* Opens the cliprdr channel on @vcm. NULL if there is no @selection, or the
 * client did not ask for the channel; the session goes on without a shared
 * clipboard. Runs on the thread-default main context. */
struct clipboard *clipboard_new(HANDLE vcm, rdpContext *context, struct selection *selection);
void clipboard_free(struct clipboard *clipboard);

#endif
