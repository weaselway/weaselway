/*
 * weaselwayd's log: GLib's, written as one "domain: message" line to stderr.
 * Every source file names its part in G_LOG_DOMAIN ("rdp", "audio", ...).
 */

#ifndef WEASELWAY_LOG_H
#define WEASELWAY_LOG_H

#include <stdbool.h>

/* @verbose lets g_debug() through, for every domain. G_MESSAGES_DEBUG does
 * the same for the domains it names. */
void log_init(bool verbose);

#endif
