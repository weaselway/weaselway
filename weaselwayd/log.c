#include "log.h"

#include <stdio.h>
#include <unistd.h>

#include <glib.h>

/* Under systemd the line starts with the level as sd-daemon(3) spells it, so
 * that the journal knows a warning from a message. */
static bool to_journal;

static GLogWriterOutput
log_writer(GLogLevelFlags level, const GLogField *fields, gsize n_fields, gpointer user_data)
{
    const char *domain = NULL, *message = NULL;
    int priority;

    for (gsize i = 0; i < n_fields; i++) {
        if (!g_strcmp0(fields[i].key, "GLIB_DOMAIN"))
            domain = fields[i].value;
        else if (!g_strcmp0(fields[i].key, "MESSAGE"))
            message = fields[i].value;
    }
    if (!message || g_log_writer_default_would_drop(level, domain))
        return G_LOG_WRITER_HANDLED;

    if (level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL))
        priority = 3;
    else if (level & G_LOG_LEVEL_WARNING)
        priority = 4;
    else if (level & G_LOG_LEVEL_MESSAGE)
        priority = 5;
    else if (level & G_LOG_LEVEL_INFO)
        priority = 6;
    else
        priority = 7;

    if (to_journal)
        fprintf(stderr, "<%d>%s: %s\n", priority, domain ? domain : "weaselwayd", message);
    else
        fprintf(stderr, "%s: %s\n", domain ? domain : "weaselwayd", message);
    return G_LOG_WRITER_HANDLED;
}

void
log_init(bool verbose)
{
    to_journal = g_log_writer_is_journald(STDERR_FILENO);
    g_log_set_writer_func(log_writer, NULL, NULL);
    if (verbose)
        g_log_set_debug_enabled(TRUE);
}
