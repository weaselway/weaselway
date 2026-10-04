/*
 * selection.c on its own, for trying it against a running mutter:
 *
 *   selection-tool watch             print what the session's clipboard holds
 *   selection-tool read MIME         print what its owner has, once it has that
 *   selection-tool offer MIME FILE   own the clipboard, with FILE's content
 *
 * tests/selection-mutter.sh runs two of these against a headless mutter.
 */

#include <stdio.h>
#include <string.h>

#include <glib-unix.h>

#include "selection.h"

static GMainLoop *loop;
static struct selection *selection;
static const char *read_mime_type, *offer_mime_type;
static GBytes *offer_data;
static int status = 1;
static gboolean reading;

static void
on_read(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) bytes = selection_read_finish(result, &error);

    if (bytes) {
        fwrite(g_bytes_get_data(bytes, NULL), 1, g_bytes_get_size(bytes), stdout);
        fflush(stdout);
        status = 0;
    } else {
        g_printerr("read failed: %s\n", error->message);
    }
    g_main_loop_quit(loop);
}

static void
on_owner_changed(void *data, const char *const *mime_types)
{
    g_autofree char *list = g_strjoinv(", ", (char **)mime_types);

    g_printerr("owner: [%s]\n", list);
    if (read_mime_type && !reading && g_strv_contains(mime_types, read_mime_type)) {
        reading = TRUE;
        selection_read_async(selection, read_mime_type, NULL, on_read, NULL);
    }
}

static void
on_requested(void *data, const char *mime_type, uint32_t serial)
{
    g_printerr("requested: %s (%u)\n", mime_type, serial);
    selection_reply(selection, serial,
                    !g_strcmp0(mime_type, offer_mime_type) ? offer_data : NULL);
}

static const struct selection_listener listener = { on_owner_changed, on_requested };

static gboolean
on_signal(gpointer data)
{
    g_main_loop_quit(loop);
    return G_SOURCE_CONTINUE;
}

int
main(int argc, char **argv)
{
    const char *offer[] = { NULL, NULL };

    if (argc == 2 && !strcmp(argv[1], "watch")) {
        status = 0;
    } else if (argc == 3 && !strcmp(argv[1], "read")) {
        read_mime_type = argv[2];
    } else if (argc == 4 && !strcmp(argv[1], "offer")) {
        g_autoptr(GError) error = NULL;
        char *contents;
        gsize length;

        if (!g_file_get_contents(argv[3], &contents, &length, &error)) {
            g_printerr("%s\n", error->message);
            return 1;
        }
        offer_mime_type = offer[0] = argv[2];
        offer_data = g_bytes_new_take(contents, length);
        status = 0;
    } else {
        g_printerr("usage: %s watch | read MIME | offer MIME FILE\n", argv[0]);
        return 2;
    }

    loop = g_main_loop_new(NULL, FALSE);
    selection = selection_new();
    selection_set_listener(selection, &listener, NULL);
    if (offer_data)
        selection_offer(selection, offer);

    g_unix_signal_add(SIGINT, on_signal, NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_main_loop_run(loop);

    selection_free(selection);
    g_clear_pointer(&offer_data, g_bytes_unref);
    g_main_loop_unref(loop);
    return status;
}
