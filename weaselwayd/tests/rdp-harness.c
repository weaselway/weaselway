/*
 * weaselwayd without a screen: the RDP server and the clipboard on a TCP
 * port, for a FreeRDP client that is not weaselway's to connect to. There is
 * no dxgdrm here and so nothing to show; a client without gfxredir is dropped
 * after a few seconds, which is long enough to copy and paste.
 *
 *   rdp-harness PORT
 *
 * tests/clipboard-rdp.sh runs it.
 */

#include <stdlib.h>

#include <gio/gio.h>
#include <glib-unix.h>

#include "log.h"
#include "rdp.h"
#include "selection.h"

static struct rdp_server *server;

/* Stand in for the compositor: the screen takes whatever size is asked for. */
static void
on_changed(void *data)
{
    int width, height;

    if (rdp_server_take_size_request(server, &width, &height))
        rdp_server_set_screen_size(server, width, height);
    rdp_server_state(server);
}

static gboolean
on_signal(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_CONTINUE;
}

int
main(int argc, char **argv)
{
    struct rdp_config config = { .shm_dir = "/dev/shm", .verbose = true, .changed = on_changed };
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_autoptr(GError) error = NULL;

    if (argc != 2 || (config.tcp_port = atoi(argv[1])) <= 0) {
        g_printerr("usage: %s PORT\n", argv[0]);
        return 2;
    }

    log_init(true);
    config.selection = selection_new();
    server = rdp_server_new(&config, &error);
    if (!server) {
        g_printerr("%s\n", error->message);
        return 1;
    }

    g_unix_signal_add(SIGINT, on_signal, loop);
    g_unix_signal_add(SIGTERM, on_signal, loop);
    g_main_loop_run(loop);

    rdp_server_free(server);
    selection_free(config.selection);
    return 0;
}
