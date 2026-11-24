/* See inhibit.h. */

#include "inhibit.h"

#include <stdbool.h>
#include <string.h>

#include <gio/gio.h>

#include "wwdisplay.h"

/* gnome-session's GsmInhibitorFlag. */
#define GSM_INHIBITOR_FLAG_SUSPEND 4
#define GSM_INHIBITOR_FLAG_IDLE 8
/* PowerDevil's PolicyAgent::RequiredPolicy. */
#define POWERDEVIL_INTERRUPT_SESSION 1
#define POWERDEVIL_CHANGE_SCREEN_SETTINGS 4

enum source_id { GNOME, POWERDEVIL, LOGIND, N_SOURCES };

static const struct {
    GBusType bus;
    const char *name, *path, *interface;
} source_info[N_SOURCES] = {
    [GNOME] = { G_BUS_TYPE_SESSION, "org.gnome.SessionManager", "/org/gnome/SessionManager",
                "org.gnome.SessionManager" },
    [POWERDEVIL] = { G_BUS_TYPE_SESSION, "org.kde.Solid.PowerManagement.PolicyAgent",
                     "/org/kde/Solid/PowerManagement/PolicyAgent",
                     "org.kde.Solid.PowerManagement.PolicyAgent" },
    [LOGIND] = { G_BUS_TYPE_SYSTEM, "org.freedesktop.login1", "/org/freedesktop/login1",
                 "org.freedesktop.login1.Manager" },
};

struct source {
    struct inhibit *inhibit;
    enum source_id id;
    GDBusProxy *proxy;
    bool idle, sleep;
};

struct inhibit {
    GCancellable *cancellable;
    struct source sources[N_SOURCES];
    uint32_t flags;
    void (*changed)(void *data, uint32_t flags);
    void *data;
};

static void
update(struct inhibit *inhibit)
{
    uint32_t flags = 0;

    for (int i = 0; i < N_SOURCES; i++) {
        /* An idle desktop is what suspends after a while. */
        if (inhibit->sources[i].idle)
            flags |= WWDISPLAY_INHIBIT_SYSTEM | WWDISPLAY_INHIBIT_DISPLAY;
        if (inhibit->sources[i].sleep)
            flags |= WWDISPLAY_INHIBIT_SYSTEM;
    }
    if (flags == inhibit->flags)
        return;

    g_message("%s", flags & WWDISPLAY_INHIBIT_DISPLAY
                         ? "the session asks to keep Windows awake and the display on"
                     : flags ? "the session asks to keep Windows awake"
                             : "the session no longer asks to keep Windows awake");
    inhibit->flags = flags;
    inhibit->changed(inhibit->data, flags);
}

/* @list is one of logind's colon separated lists. */
static bool
has_what(const char *list, const char *what)
{
    g_auto(GStrv) parts = g_strsplit(list, ":", -1);

    return g_strv_contains((const char *const *)parts, what);
}

static void
read_properties(struct source *source)
{
    GDBusProxy *proxy = source->proxy;

    source->idle = source->sleep = false;
    if (source->id == GNOME) {
        g_autoptr(GVariant) actions = g_dbus_proxy_get_cached_property(proxy, "InhibitedActions");

        if (actions && g_variant_is_of_type(actions, G_VARIANT_TYPE_UINT32)) {
            source->idle = g_variant_get_uint32(actions) & GSM_INHIBITOR_FLAG_IDLE;
            source->sleep = g_variant_get_uint32(actions) & GSM_INHIBITOR_FLAG_SUSPEND;
        }
    } else {
        /* GNOME's suspend inhibitor is a block-weak one. */
        const char *const names[] = { "BlockInhibited", "BlockWeakInhibited" };

        for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
            g_autoptr(GVariant) what = g_dbus_proxy_get_cached_property(proxy, names[i]);

            if (!what || !g_variant_is_of_type(what, G_VARIANT_TYPE_STRING))
                continue;
            source->idle |= has_what(g_variant_get_string(what, NULL), "idle");
            source->sleep |= has_what(g_variant_get_string(what, NULL), "sleep");
        }
    }
    update(source->inhibit);
}

static void
on_has_inhibition(GObject *object, GAsyncResult *result, gpointer data, bool *into)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(object), result, &error);
    gboolean has = FALSE;

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    /* Also when PowerDevil is gone. */
    if (reply)
        g_variant_get(reply, "(b)", &has);
    *into = has;
    update(((struct source *)data)->inhibit);
}

static void
on_has_idle(GObject *object, GAsyncResult *result, gpointer data)
{
    on_has_inhibition(object, result, data, &((struct source *)data)->idle);
}

static void
on_has_sleep(GObject *object, GAsyncResult *result, gpointer data)
{
    on_has_inhibition(object, result, data, &((struct source *)data)->sleep);
}

/* PowerDevil's properties are lists to make sense of; it answers the
 * question itself. */
static void
query_powerdevil(struct source *source)
{
    GCancellable *cancellable = source->inhibit->cancellable;

    g_dbus_proxy_call(source->proxy, "HasInhibition",
                      g_variant_new("(u)", POWERDEVIL_CHANGE_SCREEN_SETTINGS),
                      G_DBUS_CALL_FLAGS_NONE, -1, cancellable, on_has_idle, source);
    g_dbus_proxy_call(source->proxy, "HasInhibition",
                      g_variant_new("(u)", POWERDEVIL_INTERRUPT_SESSION), G_DBUS_CALL_FLAGS_NONE,
                      -1, cancellable, on_has_sleep, source);
}

/* For "g-properties-changed", "g-signal" and "notify::g-name-owner": the
 * proxy drops its properties when the name goes, and reads them again when
 * it comes back. */
static void
on_changed(struct source *source)
{
    if (source->id == POWERDEVIL)
        query_powerdevil(source);
    else
        read_properties(source);
}

static void
on_proxy(GObject *object, GAsyncResult *result, gpointer data)
{
    g_autoptr(GError) error = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_finish(result, &error);
    struct source *source = data;

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    if (!proxy) {
        g_warning("cannot watch %s: %s", source_info[source->id].name, error->message);
        return;
    }

    source->proxy = proxy;
    g_signal_connect_swapped(proxy, "g-properties-changed", G_CALLBACK(on_changed), source);
    g_signal_connect_swapped(proxy, "notify::g-name-owner", G_CALLBACK(on_changed), source);
    if (source->id == POWERDEVIL)
        g_signal_connect_swapped(proxy, "g-signal", G_CALLBACK(on_changed), source);
    on_changed(source);
}

static void
on_bus(GObject *object, GAsyncResult *result, gpointer data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) connection = g_bus_get_finish(result, &error);
    struct source *source = data;

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    if (!connection) {
        g_warning("cannot watch %s: %s", source_info[source->id].name, error->message);
        return;
    }

    /* weaselwayd is not to exit because a bus went. */
    g_dbus_connection_set_exit_on_close(connection, FALSE);
    g_dbus_proxy_new(connection, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL,
                     source_info[source->id].name, source_info[source->id].path,
                     source_info[source->id].interface, source->inhibit->cancellable, on_proxy,
                     source);
}

struct inhibit *
inhibit_new(void (*changed)(void *data, uint32_t flags), void *data)
{
    struct inhibit *inhibit = g_new0(struct inhibit, 1);

    inhibit->cancellable = g_cancellable_new();
    inhibit->changed = changed;
    inhibit->data = data;
    for (int i = 0; i < N_SOURCES; i++) {
        inhibit->sources[i] = (struct source){ .inhibit = inhibit, .id = i };
        g_bus_get(source_info[i].bus, inhibit->cancellable, on_bus, &inhibit->sources[i]);
    }
    return inhibit;
}

void
inhibit_free(struct inhibit *inhibit)
{
    if (!inhibit)
        return;

    g_cancellable_cancel(inhibit->cancellable);
    for (int i = 0; i < N_SOURCES; i++) {
        if (!inhibit->sources[i].proxy)
            continue;
        g_signal_handlers_disconnect_by_data(inhibit->sources[i].proxy, &inhibit->sources[i]);
        g_object_unref(inhibit->sources[i].proxy);
    }
    g_object_unref(inhibit->cancellable);
    g_free(inhibit);
}
