/*
 * See clipboard.h. Two directions, each in two steps, because neither side
 * sends what was copied before somebody pastes it:
 *
 *   The client copies:   its format list makes us take the session's
 *                        clipboard (selection_offer). When something in the
 *                        session pastes, the data is requested from the
 *                        client and passed on (selection_reply).
 *   The session copies:  the listener hears of it and the client gets a
 *                        format list. When something on Windows pastes, the
 *                        client requests the data, which is read from the
 *                        session's owner (selection_read_async).
 *
 * On the wire a format is a Windows clipboard format, in the session a mime
 * type; formats.c converts.
 *
 * The channel has no thread of its own. FreeRDP's would run these callbacks
 * off the main loop, so its event handle is watched there instead, as mutter's
 * RDP backend did, which this is ported from.
 */

#define G_LOG_DOMAIN "clipboard"

#include "clipboard.h"
#include "formats.h"
#include "selection.h"

#include <stdbool.h>
#include <string.h>

#include <glib-unix.h>

#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/server/cliprdr.h>
#include <winpr/synch.h>
#include <winpr/user.h>

/* What crosses: each of these is one format on the wire and a few mime types
 * in the session. */
enum kind {
    KIND_TEXT,
    KIND_HTML,
    KIND_IMAGE,
    N_KINDS,
};

/* The session's mime types of each kind, the one to prefer first. */
static const char *const text_mime_types[] = { "text/plain;charset=utf-8", "UTF8_STRING",
                                               "text/plain", "TEXT", "STRING", NULL };
static const char *const html_mime_types[] = { "text/html", NULL };
static const char *const image_mime_types[] = { "image/png", "image/bmp", NULL };
static const char *const *const kind_mime_types[N_KINDS] = {
    [KIND_TEXT] = text_mime_types,
    [KIND_HTML] = html_mime_types,
    [KIND_IMAGE] = image_mime_types,
};

/* What the client's clipboard is offered to the session as. */
static const char *const text_offer[] = { "text/plain;charset=utf-8", "text/plain", NULL };
static const char *const *const kind_offer[N_KINDS] = {
    [KIND_TEXT] = text_offer,
    [KIND_HTML] = html_mime_types,
    [KIND_IMAGE] = image_mime_types,
};

/* HTML is a format Windows applications register by name, so its number is
 * whatever the side announcing it says. This is ours. */
#define HTML_FORMAT_NAME "HTML Format"
#define HTML_FORMAT_ID 0xD010

/* How long the client gets to come up with what it announced, and the
 * session's owner with what it did. */
#define CLIENT_TIMEOUT_MS 10000
#define SESSION_TIMEOUT_MS 10000

/* Something in the session is pasting. */
struct request {
    uint32_t serial;
    enum kind kind;
    char *mime_type;
};

/* Something on Windows is pasting, and the session's owner is being read. It
 * may outlive the clipboard. */
struct read {
    struct clipboard *clipboard; /* NULL once that is gone */
    GCancellable *cancellable;
    guint timeout;
    enum kind kind;
    char *mime_type;
};

struct clipboard {
    CliprdrServerContext *cliprdr;
    struct selection *selection;
    guint channel_watch;

    /* The client has announced its clipboard, which is what the protocol
     * starts with; before that it takes no format list from us. */
    bool synced;

    /* What the client's clipboard holds, and its number for each. */
    bool client_has[N_KINDS];
    uint32_t client_format[N_KINDS];
    /* The session's requests for that. The client answers one at a time and
     * does not say which, so only the first has been sent to it. */
    GQueue requests;
    guint request_timeout;

    /* What the session's clipboard holds: the mime type to read for each
     * kind, NULL if there is none. */
    const char *session_mime_type[N_KINDS];
    struct read *read;
};

static bool
strv_has(const char *const *strv, const char *string)
{
    return strv && g_strv_contains(strv, string);
}

static bool
kind_of_mime_type(const char *mime_type, enum kind *kind)
{
    for (int k = 0; k < N_KINDS; k++) {
        if (strv_has(kind_mime_types[k], mime_type)) {
            *kind = k;
            return true;
        }
    }
    return false;
}

static uint32_t
wire_format(enum kind kind)
{
    switch (kind) {
    case KIND_TEXT:
        return CF_UNICODETEXT;
    case KIND_HTML:
        return HTML_FORMAT_ID;
    default:
        return CF_DIB;
    }
}

static GBytes *
session_from_wire(enum kind kind, const char *mime_type, GBytes *data)
{
    switch (kind) {
    case KIND_TEXT:
        return formats_text_from_unicode(data);
    case KIND_HTML:
        return formats_html_from_cf_html(data);
    default:
        return g_str_equal(mime_type, "image/png") ? formats_png_from_dib(data)
                                                   : formats_bmp_from_dib(data);
    }
}

static GBytes *
wire_from_session(enum kind kind, const char *mime_type, GBytes *data)
{
    switch (kind) {
    case KIND_TEXT:
        return formats_text_to_unicode(data);
    case KIND_HTML:
        return formats_html_to_cf_html(data);
    default:
        return g_str_equal(mime_type, "image/png") ? formats_png_to_dib(data)
                                                   : formats_bmp_to_dib(data);
    }
}

/* ------------------------------------------------------------------ */
/* The session's clipboard, for the client                             */
/* ------------------------------------------------------------------ */

/* Tell the client what it can paste from the session. An empty list is how
 * it learns that there is nothing any more. */
static void
send_format_list(struct clipboard *clipboard)
{
    CLIPRDR_FORMAT formats[N_KINDS] = { 0 };
    CLIPRDR_FORMAT_LIST list = { .common.msgType = CB_FORMAT_LIST, .formats = formats };

    for (int k = 0; k < N_KINDS; k++) {
        if (!clipboard->session_mime_type[k])
            continue;
        formats[list.numFormats].formatId = wire_format(k);
        formats[list.numFormats].formatName = k == KIND_HTML ? (char *)HTML_FORMAT_NAME : NULL;
        list.numFormats++;
    }

    g_debug("announcing %u format(s) of the session's clipboard", (unsigned)list.numFormats);
    if (clipboard->cliprdr->ServerFormatList(clipboard->cliprdr, &list) != CHANNEL_RC_OK)
        g_warning("cannot announce the session's clipboard to the client");
}

static void
set_session_mime_types(struct clipboard *clipboard, const char *const *mime_types)
{
    for (int k = 0; k < N_KINDS; k++) {
        clipboard->session_mime_type[k] = NULL;
        for (const char *const *want = kind_mime_types[k]; *want; want++) {
            if (strv_has(mime_types, *want)) {
                /* The table's string, which stays. */
                clipboard->session_mime_type[k] = *want;
                break;
            }
        }
    }
}

static void
on_owner_changed(void *data, const char *const *mime_types)
{
    struct clipboard *clipboard = data;

    set_session_mime_types(clipboard, mime_types);
    if (clipboard->synced)
        send_format_list(clipboard);
}

static void
send_data_response(struct clipboard *clipboard, GBytes *data)
{
    CLIPRDR_FORMAT_DATA_RESPONSE response = {
        .common.msgType = CB_FORMAT_DATA_RESPONSE,
        .common.msgFlags = data ? CB_RESPONSE_OK : CB_RESPONSE_FAIL,
    };
    gsize size = 0;

    if (data)
        response.requestedFormatData = g_bytes_get_data(data, &size);
    response.common.dataLen = (UINT32)size;

    if (clipboard->cliprdr->ServerFormatDataResponse(clipboard->cliprdr, &response) !=
        CHANNEL_RC_OK)
        g_warning("cannot send clipboard data to the client");
}

static void
read_free(struct read *read)
{
    g_clear_handle_id(&read->timeout, g_source_remove);
    g_object_unref(read->cancellable);
    g_free(read->mime_type);
    g_free(read);
}

static void
on_session_read(GObject *source, GAsyncResult *result, gpointer data)
{
    struct read *read = data;
    struct clipboard *clipboard = read->clipboard;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) bytes = selection_read_finish(result, &error);
    g_autoptr(GBytes) wire = NULL;

    if (clipboard) {
        clipboard->read = NULL;

        if (!bytes)
            g_message("cannot read %s from the session's clipboard: %s", read->mime_type,
                      error->message);
        else if (g_bytes_get_size(bytes) > G_MAXINT32 ||
                 !(wire = wire_from_session(read->kind, read->mime_type, bytes)) ||
                 g_bytes_get_size(wire) > G_MAXINT32)
            g_message("cannot convert the %zu bytes of %s on the session's clipboard",
                      g_bytes_get_size(bytes), read->mime_type);
        else
            g_debug("%zu bytes of %s go to the client", g_bytes_get_size(bytes), read->mime_type);

        send_data_response(clipboard, wire);
    }

    read_free(read);
}

static gboolean
on_read_timeout(gpointer data)
{
    struct read *read = data;

    read->timeout = 0;
    g_cancellable_cancel(read->cancellable);
    return G_SOURCE_REMOVE;
}

/* The client is pasting something of the session's. */
static UINT
on_client_format_data_request(CliprdrServerContext *context,
                              const CLIPRDR_FORMAT_DATA_REQUEST *request)
{
    struct clipboard *clipboard = context->custom;
    struct read *read;
    int kind;

    for (kind = 0; kind < N_KINDS; kind++) {
        if (wire_format(kind) == request->requestedFormatId)
            break;
    }

    /* One at a time, which is all a client asks for and all mutter does. */
    if (kind == N_KINDS || !clipboard->session_mime_type[kind] || clipboard->read) {
        g_debug("the client asked for format 0x%x, which is not to be had",
                (unsigned)request->requestedFormatId);
        send_data_response(clipboard, NULL);
        return CHANNEL_RC_OK;
    }

    read = g_new0(struct read, 1);
    read->clipboard = clipboard;
    read->cancellable = g_cancellable_new();
    read->kind = kind;
    read->mime_type = g_strdup(clipboard->session_mime_type[kind]);
    /* An owner that never closes its end of the pipe would otherwise keep
     * the client waiting for good. */
    read->timeout = g_timeout_add(SESSION_TIMEOUT_MS, on_read_timeout, read);
    clipboard->read = read;

    selection_read_async(clipboard->selection, read->mime_type, read->cancellable,
                         on_session_read, read);
    return CHANNEL_RC_OK;
}

/* ------------------------------------------------------------------ */
/* The client's clipboard, for the session                             */
/* ------------------------------------------------------------------ */

static void
request_free(struct request *request)
{
    g_free(request->mime_type);
    g_free(request);
}

static void ask_client(struct clipboard *clipboard);

/* The first request is over: answer it, and go on to the next. */
static void
finish_request(struct clipboard *clipboard, GBytes *data)
{
    struct request *request = g_queue_pop_head(&clipboard->requests);

    g_clear_handle_id(&clipboard->request_timeout, g_source_remove);
    if (!request)
        return;

    selection_reply(clipboard->selection, request->serial, data);
    request_free(request);
    ask_client(clipboard);
}

static gboolean
on_request_timeout(gpointer data)
{
    struct clipboard *clipboard = data;

    clipboard->request_timeout = 0;
    g_message("the client did not come up with its clipboard's data");
    finish_request(clipboard, NULL);
    return G_SOURCE_REMOVE;
}

/* Send the first request to the client, if there is one. */
static void
ask_client(struct clipboard *clipboard)
{
    struct request *request;

    while ((request = g_queue_peek_head(&clipboard->requests))) {
        CLIPRDR_FORMAT_DATA_REQUEST data_request = {
            .common.msgType = CB_FORMAT_DATA_REQUEST,
            .common.dataLen = 4,
            .requestedFormatId = clipboard->client_format[request->kind],
        };

        if (clipboard->client_has[request->kind] &&
            clipboard->cliprdr->ServerFormatDataRequest(clipboard->cliprdr, &data_request) ==
                CHANNEL_RC_OK) {
            clipboard->request_timeout =
                g_timeout_add(CLIENT_TIMEOUT_MS, on_request_timeout, clipboard);
            return;
        }

        /* The client's clipboard has moved on. */
        g_queue_pop_head(&clipboard->requests);
        selection_reply(clipboard->selection, request->serial, NULL);
        request_free(request);
    }
}

/* Something in the session is pasting what the client has. */
static void
on_requested(void *data, const char *mime_type, uint32_t serial)
{
    struct clipboard *clipboard = data;
    struct request *request;
    enum kind kind;

    if (!kind_of_mime_type(mime_type, &kind) || !clipboard->client_has[kind]) {
        selection_reply(clipboard->selection, serial, NULL);
        return;
    }

    request = g_new0(struct request, 1);
    request->serial = serial;
    request->kind = kind;
    request->mime_type = g_strdup(mime_type);
    g_queue_push_tail(&clipboard->requests, request);

    if (g_queue_get_length(&clipboard->requests) == 1)
        ask_client(clipboard);
}

static UINT
on_client_format_data_response(CliprdrServerContext *context,
                               const CLIPRDR_FORMAT_DATA_RESPONSE *response)
{
    struct clipboard *clipboard = context->custom;
    struct request *request = g_queue_peek_head(&clipboard->requests);
    g_autoptr(GBytes) wire = NULL;
    g_autoptr(GBytes) data = NULL;

    /* Nobody is waiting for it. */
    if (!request || !clipboard->request_timeout)
        return CHANNEL_RC_OK;

    if ((response->common.msgFlags & CB_RESPONSE_OK) && response->requestedFormatData) {
        wire = g_bytes_new(response->requestedFormatData, response->common.dataLen);
        data = session_from_wire(request->kind, request->mime_type, wire);
        if (!data)
            g_message("cannot convert the %u bytes the client sent to %s",
                      (unsigned)response->common.dataLen, request->mime_type);
        else
            g_debug("%zu bytes of %s go to the session", g_bytes_get_size(data),
                    request->mime_type);
    } else {
        g_debug("the client has no data for %s", request->mime_type);
    }

    finish_request(clipboard, data);
    return CHANNEL_RC_OK;
}

/* The client copied something, or connected with something on its
 * clipboard, or has nothing on it any more. */
static UINT
on_client_format_list(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST *list)
{
    struct clipboard *clipboard = context->custom;
    CLIPRDR_FORMAT_LIST_RESPONSE response = {
        .common.msgType = CB_FORMAT_LIST_RESPONSE,
        .common.msgFlags = CB_RESPONSE_OK,
    };
    g_autoptr(GPtrArray) offer = g_ptr_array_new();
    bool first = !clipboard->synced;

    memset(clipboard->client_has, 0, sizeof(clipboard->client_has));
    for (UINT32 i = 0; i < list->numFormats; i++) {
        const CLIPRDR_FORMAT *format = &list->formats[i];
        int kind;

        if (format->formatName && format->formatName[0]) {
            if (g_ascii_strcasecmp(format->formatName, HTML_FORMAT_NAME))
                continue;
            kind = KIND_HTML;
        } else if (format->formatId == CF_UNICODETEXT) {
            kind = KIND_TEXT;
        } else if (format->formatId == CF_DIB) {
            kind = KIND_IMAGE;
        } else {
            continue;
        }

        clipboard->client_has[kind] = true;
        clipboard->client_format[kind] = format->formatId;
    }

    if (context->ServerFormatListResponse(context, &response) != CHANNEL_RC_OK)
        g_warning("cannot acknowledge the client's clipboard");

    for (int k = 0; k < N_KINDS; k++) {
        if (!clipboard->client_has[k])
            continue;
        for (const char *const *mime_type = kind_offer[k]; *mime_type; mime_type++)
            g_ptr_array_add(offer, (gpointer)*mime_type);
    }
    g_debug("the client announced %u format(s), %u of which the session gets",
            (unsigned)list->numFormats, offer->len);
    g_ptr_array_add(offer, NULL);

    clipboard->synced = true;
    selection_offer(clipboard->selection, (const char *const *)offer->pdata);

    /* A client that connects with nothing of interest on its clipboard gets
     * what the session has, if that is anything. One that has something
     * overrides it, as it would have had it been connected when that was
     * copied. */
    if (first && offer->len == 1) {
        for (int k = 0; k < N_KINDS; k++) {
            if (clipboard->session_mime_type[k]) {
                send_format_list(clipboard);
                break;
            }
        }
    }
    return CHANNEL_RC_OK;
}

static UINT
on_client_format_list_response(CliprdrServerContext *context,
                               const CLIPRDR_FORMAT_LIST_RESPONSE *response)
{
    if (!(response->common.msgFlags & CB_RESPONSE_OK))
        g_debug("the client did not take the session's format list");
    return CHANNEL_RC_OK;
}

/* ------------------------------------------------------------------ */
/* The channel                                                         */
/* ------------------------------------------------------------------ */

static gboolean
on_channel_ready(gint fd, GIOCondition condition, gpointer data)
{
    struct clipboard *clipboard = data;
    HANDLE event = clipboard->cliprdr->GetEventHandle(clipboard->cliprdr);

    /* Only while something is queued: FreeRDP reports a read from an empty
     * queue like a protocol error. */
    while (WaitForSingleObject(event, 0) == WAIT_OBJECT_0) {
        UINT status = clipboard->cliprdr->CheckEventHandle(clipboard->cliprdr);

        if (status != CHANNEL_RC_OK) {
            g_warning("the clipboard channel failed (0x%x); no clipboard for this client",
                      (unsigned)status);
            clipboard->channel_watch = 0;
            return G_SOURCE_REMOVE;
        }
    }
    return G_SOURCE_CONTINUE;
}

/* What FreeRDP's own thread would open the channel with: our capabilities,
 * and the go-ahead for the client to announce its clipboard. */
static bool
send_greeting(CliprdrServerContext *cliprdr)
{
    CLIPRDR_GENERAL_CAPABILITY_SET general = {
        .capabilitySetType = CB_CAPSTYPE_GENERAL,
        .capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN,
        .version = CB_CAPS_VERSION_2,
        .generalFlags = CB_USE_LONG_FORMAT_NAMES,
    };
    CLIPRDR_CAPABILITIES capabilities = {
        .common.msgType = CB_CLIP_CAPS,
        .common.dataLen = 4 + CB_CAPSTYPE_GENERAL_LEN,
        .cCapabilitiesSets = 1,
        .capabilitySets = (CLIPRDR_CAPABILITY_SET *)&general,
    };
    CLIPRDR_MONITOR_READY monitor_ready = { .common.msgType = CB_MONITOR_READY };

    return cliprdr->ServerCapabilities(cliprdr, &capabilities) == CHANNEL_RC_OK &&
           cliprdr->MonitorReady(cliprdr, &monitor_ready) == CHANNEL_RC_OK;
}

static const struct selection_listener listener = {
    .owner_changed = on_owner_changed,
    .requested = on_requested,
};

struct clipboard *
clipboard_new(HANDLE vcm, rdpContext *context, struct selection *selection)
{
    struct clipboard *clipboard;
    CliprdrServerContext *cliprdr;
    int fd;

    if (!selection || !vcm || vcm == INVALID_HANDLE_VALUE)
        return NULL;

    if (!WTSVirtualChannelManagerIsChannelJoined(vcm, CLIPRDR_SVC_CHANNEL_NAME)) {
        g_message("the client did not ask for a clipboard");
        return NULL;
    }

    cliprdr = cliprdr_server_context_new(vcm);
    if (!cliprdr) {
        g_warning("cliprdr_server_context_new failed");
        return NULL;
    }

    clipboard = g_new0(struct clipboard, 1);
    clipboard->cliprdr = cliprdr;
    clipboard->selection = selection;
    g_queue_init(&clipboard->requests);

    cliprdr->custom = clipboard;
    cliprdr->rdpcontext = context;
    /* The client's capabilities need no callback: FreeRDP takes note of what
     * matters, long format names, by itself. */
    cliprdr->ClientFormatList = on_client_format_list;
    cliprdr->ClientFormatListResponse = on_client_format_list_response;
    cliprdr->ClientFormatDataRequest = on_client_format_data_request;
    cliprdr->ClientFormatDataResponse = on_client_format_data_response;

    /* Names are how HTML is recognised. No files. */
    cliprdr->useLongFormatNames = TRUE;
    cliprdr->streamFileClipEnabled = FALSE;
    cliprdr->fileClipNoFilePaths = TRUE;
    cliprdr->canLockClipData = FALSE;
    cliprdr->hasHugeFileSupport = FALSE;

    /* Open(), and not Start(), which is what would spawn the thread. */
    if (cliprdr->Open(cliprdr) != CHANNEL_RC_OK) {
        g_warning("cannot open the clipboard channel");
        cliprdr_server_context_free(cliprdr);
        g_free(clipboard);
        return NULL;
    }

    fd = GetEventFileDescriptor(cliprdr->GetEventHandle(cliprdr));
    if (fd < 0 || !send_greeting(cliprdr)) {
        g_warning("cannot start the clipboard channel");
        cliprdr->Close(cliprdr);
        cliprdr_server_context_free(cliprdr);
        g_free(clipboard);
        return NULL;
    }
    clipboard->channel_watch = g_unix_fd_add(fd, G_IO_IN, on_channel_ready, clipboard);

    set_session_mime_types(clipboard, selection_get_mime_types(selection));
    selection_set_listener(selection, &listener, clipboard);

    g_message("clipboard channel ready");
    return clipboard;
}

void
clipboard_free(struct clipboard *clipboard)
{
    struct request *request;

    if (!clipboard)
        return;

    /* What the client had cannot be pasted without it. */
    selection_set_listener(clipboard->selection, NULL, NULL);
    while ((request = g_queue_pop_head(&clipboard->requests))) {
        selection_reply(clipboard->selection, request->serial, NULL);
        request_free(request);
    }
    selection_offer(clipboard->selection, NULL);

    g_clear_handle_id(&clipboard->request_timeout, g_source_remove);
    g_clear_handle_id(&clipboard->channel_watch, g_source_remove);
    if (clipboard->read) {
        clipboard->read->clipboard = NULL;
        g_cancellable_cancel(clipboard->read->cancellable);
    }

    clipboard->cliprdr->Close(clipboard->cliprdr);
    cliprdr_server_context_free(clipboard->cliprdr);
    g_free(clipboard);
}
