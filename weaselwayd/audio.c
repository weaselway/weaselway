/*
 * See audio.h. Ported from mutter's src/backends/rdp/meta-rdp-audio.c, whose
 * RDP half in turn comes from wslg's rdpaudio.c and rdpaudioin.c.
 *
 * The Linux half is a plain TCP client of two stock
 * libpipewire-module-protocol-simple servers. What crosses those sockets is
 * raw interleaved PCM and nothing else, so the formats below have to agree
 * with pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf on rate, channels
 * and sample format: protocol-simple fixes them when the module is loaded and
 * can neither negotiate nor announce them.
 *
 * Two consequences of there being no framing:
 *
 *   - There is no latency feedback from the sink, so the client is given no
 *     timestamps to synchronise video with.
 *   - We are a client, so we retry: PipeWire may not be up when a viewer
 *     connects, and it drops our streams whenever it restarts.
 */

#define G_LOG_DOMAIN "audio"

#include "audio.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

#include <freerdp/channels/channels.h>
#include <freerdp/server/audin.h>
#include <freerdp/server/rdpsnd.h>
#include <winpr/stream.h>

/* Where the protocol-simple servers listen; server.address in the PipeWire
 * drop-in. The variables are for pointing the bridge at a PipeWire somewhere
 * else while debugging. */
#define SINK_ADDR_ENV "WEASELWAY_AUDIO_SINK_ADDR"
#define SINK_ADDR_DEFAULT "127.0.0.1:4711"

#define SOURCE_ADDR_ENV "WEASELWAY_AUDIO_SOURCE_ADDR"
#define SOURCE_ADDR_DEFAULT "127.0.0.1:4712"

/* How long to wait between connection attempts, and how often to look
 * whether the capture connection is still there. */
#define RECONNECT_INTERVAL_MS 1000
#define POLL_INTERVAL_MS 500

/* ------------------------------------------------------------------ */
/* Shared: interruptible waiting and connecting                        */
/* ------------------------------------------------------------------ */

/* Waits up to @timeout_ms. False as soon as @cancellable is cancelled, so
 * every sleep in this file is also a check for teardown; a thread could
 * otherwise not be joined before its timeout happened to run out. */
static bool
wait_unless_cancelled(GCancellable *cancellable, int timeout_ms)
{
    GPollFD pfd;

    if (g_cancellable_make_pollfd(cancellable, &pfd)) {
        g_poll(&pfd, 1, timeout_ms);
        g_cancellable_release_fd(cancellable);
    } else {
        g_usleep((gulong)timeout_ms * 1000);
    }

    return !g_cancellable_is_cancelled(cancellable);
}

/* Connects, trying again until it works or teardown is signalled; NULL only
 * for the latter (and for an address that cannot be one), so a caller can
 * take that as "leave the thread".
 *
 * PipeWire is a service of its own that may start after us, and it drops
 * every client stream when it restarts. */
static GSocketConnection *
connect_retrying(const char *env_var, const char *default_addr, GCancellable *cancellable,
                 const char *what)
{
    const char *spec = g_getenv(env_var);
    g_autoptr(GSocketConnectable) address = NULL;
    g_autoptr(GSocketClient) client = g_socket_client_new();
    bool warned = false;

    if (!spec || !spec[0])
        spec = default_addr;

    address = g_network_address_parse(spec, 0, NULL);
    if (!address || !g_network_address_get_port(G_NETWORK_ADDRESS(address))) {
        g_warning("malformed address '%s', want host:port", spec);
        return NULL;
    }

    /* A loopback connection; nothing to ask a proxy resolver about. */
    g_socket_client_set_enable_proxy(client, FALSE);

    while (!g_cancellable_is_cancelled(cancellable)) {
        g_autoptr(GError) error = NULL;
        GSocketConnection *connection =
            g_socket_client_connect(client, address, cancellable, &error);

        if (connection) {
            /* Both directions move packets of a few milliseconds. Nagle would
             * collect them into bursts, for no gain on a loopback connection. */
            if (!g_socket_set_option(g_socket_connection_get_socket(connection), IPPROTO_TCP,
                                     TCP_NODELAY, 1, &error))
                g_warning("TCP_NODELAY failed: %s", error->message);
            if (warned)
                g_message("%s connected to %s", what, spec);
            return connection;
        }

        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            break;

        /* Once, not once a second: a PipeWire that is not running yet is an
         * ordinary race at startup, and trying again is the handling. */
        if (!warned) {
            g_message("%s cannot reach %s (%s), retrying", what, spec, error->message);
            warned = true;
        }

        if (!wait_unless_cancelled(cancellable, RECONNECT_INTERVAL_MS))
            break;
    }

    return NULL;
}

static bool
same_format(const AUDIO_FORMAT *a, const AUDIO_FORMAT *b)
{
    return a->wFormatTag == b->wFormatTag && a->nChannels == b->nChannels &&
           a->nSamplesPerSec == b->nSamplesPerSec && a->wBitsPerSample == b->wBitsPerSample;
}

/* ------------------------------------------------------------------ */
/* Playback: PipeWire protocol-simple -> rdpsnd                        */
/* ------------------------------------------------------------------ */

/* Milliseconds of audio in an RDP packet. SendSamples() does not write to the
 * socket: it queues a PDU with the channel manager, whose event wakes the
 * main loop -- the thread that also presents the frames. Weston used 5 ms,
 * about 200 wakeups a second; 20 ms is 50, for 15 ms more latency, which is
 * well within what is normal for RDP. A variable because the right value
 * depends on the client and the machine. */
#define LATENCY_MS_DEFAULT 20
#define LATENCY_MS_ENV "WEASELWAY_AUDIO_LATENCY_MS"
#define LATENCY_MS_MIN 5
#define LATENCY_MS_MAX 200

/* Set to 1 to forward silence rather than dropping it (see forward_packet()),
 * for when a glitch has to be pinned on that or cleared of it. */
#define SEND_SILENCE_ENV "WEASELWAY_AUDIO_SEND_SILENCE"

/* Set to 1 to use the static rdpsnd channel instead of the dynamic one. */
#define DISABLE_DVC_ENV "WEASELWAY_AUDIO_DISABLE_DVC"

/* How many RDP audio blocks can be in flight at once; where
 * RdpsndServerContext::block_no wraps around. */
#define MAX_BLOCKS_IN_FLIGHT 256

/* How much audio may be queued between PipeWire and the client. Clients
 * confirm a block when it arrives, not when it has played, so the limit on
 * blocks in flight does not bound the latency by itself: after a stall the
 * backlog in the sink socket would go out in one burst and be played seconds
 * late. Both the blocks in flight and the socket's backlog are capped to
 * this, dropping the oldest audio to catch up. */
#define MAX_BUFFERED_MS 150

struct block_info {
    /* Submitted to rdpsnd and not given back yet. A confirm for a block that
     * was never submitted must not hand out a slot we do not own. */
    bool in_flight;
    /* The block's slot has been handed back already, so a duplicate or late
     * confirm cannot release it twice. */
    bool slot_released;
};

struct audio_out {
    RdpsndServerContext *rdpsnd;
    bool verbose;

    /* Cancelled at teardown, which ends every wait and read of the playback
     * thread. */
    GCancellable *cancellable;
    /* Started by the first activation, on rdpsnd's thread. */
    GMutex thread_lock;
    GThread *thread;

    int bytes_per_frame;
    /* Frames in an RDP packet, from the latency. The same as rdpsnd's own
     * out_frames, so that every SendSamples() emits exactly one PDU and keeps
     * nothing -- which is what makes dropping silent packets safe: there is
     * never half a buffer for the drop to strand. */
    int frames_per_packet;
    bool send_silence;
    uint8_t *buffer;
    size_t buffer_size;

    /* MAX_BUFFERED_MS in blocks, and in bytes of the sink socket. */
    int max_in_flight;
    size_t max_backlog_bytes;

    /* What bounds the blocks in flight, Weston's audioSem: the playback
     * thread takes a slot for every block, and the confirm, on rdpsnd's
     * thread, gives it back. */
    GMutex block_lock;
    GCond block_cond;
    int free_slots;
    struct block_info block_info[MAX_BLOCKS_IN_FLIGHT];

    audio_flush_func flush;
    void *flush_data;

    /* For the verbose log: the packets of the sound that is playing. */
    bool playing;
    unsigned sent;
    int confirmed; /* atomic */
};

static AUDIO_FORMAT audio_out_format = { WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0, NULL };

/* Takes a slot for the block rdpsnd is about to send, waiting if there is
 * none. False if teardown woke us instead; no slot was taken then. */
static bool
block_acquire(struct audio_out *audio_out, BYTE *block_no)
{
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&audio_out->block_lock);

    while (!audio_out->free_slots) {
        if (g_cancellable_is_cancelled(audio_out->cancellable))
            return false;

        /* The sink socket is not being drained while we sit here, so
         * PipeWire starts dropping what it cannot hand us. */
        if (!g_cond_wait_until(&audio_out->block_cond, &audio_out->block_lock,
                               g_get_monotonic_time() + G_TIME_SPAN_SECOND))
            g_warning("stalled waiting for the client to confirm audio blocks");
    }

    audio_out->free_slots--;
    *block_no = audio_out->rdpsnd->block_no;
    audio_out->block_info[*block_no] = (struct block_info){ .in_flight = true };
    return true;
}

/* Blocks that were unconfirmed when a connection dropped would keep their
 * slots forever, so every new connection starts with the full credit. */
static void
block_reset(struct audio_out *audio_out)
{
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&audio_out->block_lock);

    audio_out->free_slots = audio_out->max_in_flight;
    memset(audio_out->block_info, 0, sizeof(audio_out->block_info));
}

/* On rdpsnd's thread. */
static UINT
on_rdpsnd_confirm_block(RdpsndServerContext *context, BYTE confirm_block_num, UINT16 wtimestamp)
{
    struct audio_out *audio_out = context->data;
    struct block_info *info = &audio_out->block_info[confirm_block_num];
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&audio_out->block_lock);

    if (!info->in_flight) {
        g_warning("spurious confirm for block %u", confirm_block_num);
    } else if (!info->slot_released) {
        /* Release on the first confirm. Clients differ in how many they
         * send -- mstsc sends two a block once a latency is advertised
         * (received, then rendered), others one -- and waiting for a second
         * that never comes uses up every slot after 256 blocks and stops
         * playback for good. */
        info->slot_released = true;
        g_atomic_int_inc(&audio_out->confirmed);
        audio_out->free_slots++;
        g_cond_signal(&audio_out->block_cond);
    }

    return CHANNEL_RC_OK;
}

/* Drops the oldest audio queued in the sink socket beyond max_backlog_bytes,
 * in whole frames. See MAX_BUFFERED_MS. */
static void
skip_backlog(struct audio_out *audio_out, GSocketConnection *sink)
{
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(sink));
    gssize available = g_socket_get_available_bytes(g_socket_connection_get_socket(sink));
    size_t excess;

    if (available < 0 || (size_t)available <= audio_out->max_backlog_bytes)
        return;

    excess = (size_t)available - audio_out->max_backlog_bytes;
    excess -= excess % (size_t)audio_out->bytes_per_frame;

    if (audio_out->verbose)
        g_debug("dropping %zu bytes (%zu ms) of backlog", excess,
                excess / (size_t)audio_out->bytes_per_frame * 1000 /
                    audio_out_format.nSamplesPerSec);

    while (excess > 0) {
        gssize n = g_input_stream_skip(in, excess, audio_out->cancellable, NULL);

        if (n <= 0)
            return; /* the next read reports it */

        excess -= (size_t)n;
    }
}

/* Reads exactly one RDP packet of PCM from the socket and hands it to rdpsnd.
 * Reading a fixed whole number of frames keeps us aligned to frames, which the
 * stream has no other means for.
 *
 * False when the connection is gone or teardown was signalled; the caller
 * connects again or leaves. */
static bool
forward_packet(struct audio_out *audio_out, GSocketConnection *sink)
{
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(sink));
    g_autoptr(GError) error = NULL;
    size_t chunk = audio_out->buffer_size;
    gsize got = 0;
    BYTE block_no;

    skip_backlog(audio_out, sink);

    if (!g_input_stream_read_all(in, audio_out->buffer, chunk, &got, audio_out->cancellable,
                                 &error)) {
        /* Teardown cancels exactly this read. */
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning("read from playback server failed: %s", error->message);
        return false;
    }
    if (got < chunk) {
        g_message("playback server closed the connection");
        return false;
    }

    /* The sink's monitor delivers samples whenever the PipeWire graph runs,
     * so with nothing playing this is a steady stream of zeroes, and every
     * packet of it would wake the main loop to no audible end. Drop them;
     * clients cope with the stream simply stopping.
     *
     * The first byte being zero and the buffer being equal to itself shifted
     * by one means all bytes are zero. */
    if (!audio_out->send_silence && got > 1 && audio_out->buffer[0] == 0 &&
        memcmp(audio_out->buffer, audio_out->buffer + 1, got - 1) == 0) {
        if (audio_out->playing && audio_out->verbose)
            g_debug("silence after %u packets, of which the client confirmed %d",
                    audio_out->sent, g_atomic_int_get(&audio_out->confirmed));
        audio_out->playing = false;
        return true;
    }

    if (!audio_out->playing) {
        audio_out->playing = true;
        audio_out->sent = 0;
        g_atomic_int_set(&audio_out->confirmed, 0);
    }

    /* Waits until an earlier block has been confirmed. */
    if (!block_acquire(audio_out, &block_no))
        return false;

    /* Timestamp 0 turns A/V sync off at the client, as in Weston; there is
     * nothing meaningful to give it. */
    if (audio_out->rdpsnd->SendSamples(audio_out->rdpsnd, audio_out->buffer,
                                       (size_t)audio_out->frames_per_packet, 0) != CHANNEL_RC_OK) {
        g_warning("SendSamples failed");
        return false;
    }

    if (block_no == audio_out->rdpsnd->block_no) {
        /* Nothing was sent this time; the slot goes back. */
        g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&audio_out->block_lock);

        audio_out->block_info[block_no] = (struct block_info){ .slot_released = true };
        audio_out->free_slots++;
        return true;
    }

    audio_out->sent++;

    /* SendSamples() only queues. Without this the bytes wait for the main
     * loop's next turn, which is the coupling to the frames this thread is
     * there to avoid. */
    if (audio_out->flush)
        audio_out->flush(audio_out->flush_data);

    return true;
}

static gpointer
audio_out_thread(gpointer data)
{
    struct audio_out *audio_out = data;

    while (!g_cancellable_is_cancelled(audio_out->cancellable)) {
        g_autoptr(GSocketConnection) sink =
            connect_retrying(SINK_ADDR_ENV, SINK_ADDR_DEFAULT, audio_out->cancellable, "playback");

        if (!sink)
            break;

        /* Credit does not carry over: what was outstanding when the last
         * connection dropped is never going to be confirmed. */
        block_reset(audio_out);

        while (forward_packet(audio_out, sink))
            ;
    }

    return NULL;
}

/* On rdpsnd's thread. */
static void
on_rdpsnd_activated(RdpsndServerContext *context)
{
    struct audio_out *audio_out = context->data;
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&audio_out->thread_lock);
    const char *latency_env = g_getenv(LATENCY_MS_ENV);
    const AUDIO_FORMAT *client_format = NULL;
    g_autoptr(GError) error = NULL;
    int latency_ms = LATENCY_MS_DEFAULT;
    UINT16 format = 0;

    /* Clients may activate rdpsnd more than once (reconnect, format change);
     * the thread is set up on the first. */
    if (audio_out->thread || g_cancellable_is_cancelled(audio_out->cancellable))
        return;

    for (UINT16 i = 0; i < context->num_client_formats; i++) {
        if (same_format(&context->client_formats[i], &audio_out_format)) {
            client_format = &context->client_formats[i];
            format = i;
            break;
        }
    }

    if (!client_format) {
        g_warning("client and server agreed on no playback format");
        return;
    }

    audio_out->bytes_per_frame = (client_format->wBitsPerSample / 8) * client_format->nChannels;

    if (latency_env && latency_env[0]) {
        gint64 parsed;

        if (g_ascii_string_to_signed(latency_env, 10, LATENCY_MS_MIN, LATENCY_MS_MAX, &parsed,
                                     NULL))
            latency_ms = (int)parsed;
        else
            g_warning("ignoring %s='%s', want %d-%d", LATENCY_MS_ENV, latency_env, LATENCY_MS_MIN,
                      LATENCY_MS_MAX);
    }

    /* rdpsnd gets the figure we packetise at, so that its out_frames is our
     * frames_per_packet and every SendSamples() emits one PDU. */
    context->latency = (UINT32)latency_ms;
    audio_out->frames_per_packet = (int)client_format->nSamplesPerSec * latency_ms / 1000;

    audio_out->send_silence = g_strcmp0(g_getenv(SEND_SILENCE_ENV), "1") == 0;

    /* One RDP packet, which is all forward_packet() ever reads at a time. */
    audio_out->buffer_size =
        (size_t)audio_out->frames_per_packet * (size_t)audio_out->bytes_per_frame;
    g_free(audio_out->buffer);
    audio_out->buffer = g_malloc0(audio_out->buffer_size);

    audio_out->max_in_flight = MAX(MAX_BUFFERED_MS / latency_ms, 2);
    audio_out->max_backlog_bytes = (size_t)client_format->nSamplesPerSec * MAX_BUFFERED_MS /
                                   1000 * (size_t)audio_out->bytes_per_frame;

    if (context->SelectFormat(context, format) != CHANNEL_RC_OK ||
        context->SetVolume(context, 0x7FFF, 0x7FFF) != CHANNEL_RC_OK)
        g_warning("cannot select the playback format");

    g_message("playback %d ms/packet (%d frames, %.0f packets/s)%s", latency_ms,
              audio_out->frames_per_packet, 1000.0 / latency_ms,
              audio_out->send_silence ? ", forwarding silence" : "");

    audio_out->thread = g_thread_try_new("audio-out", audio_out_thread, audio_out, &error);
    if (!audio_out->thread)
        g_warning("cannot start the playback thread: %s", error->message);
}

struct audio_out *
audio_out_new(HANDLE vcm, audio_flush_func flush, void *flush_data, bool verbose)
{
    struct audio_out *audio_out;
    RdpsndServerContext *rdpsnd;
    AUDIO_FORMAT *server_formats;

    if (!vcm || vcm == INVALID_HANDLE_VALUE)
        return NULL;

    rdpsnd = rdpsnd_server_context_new(vcm);
    if (!rdpsnd) {
        g_warning("rdpsnd_server_context_new failed");
        return NULL;
    }

    audio_out = g_new0(struct audio_out, 1);
    audio_out->rdpsnd = rdpsnd;
    audio_out->verbose = verbose;
    audio_out->flush = flush;
    audio_out->flush_data = flush_data;
    audio_out->cancellable = g_cancellable_new();
    g_mutex_init(&audio_out->thread_lock);
    g_mutex_init(&audio_out->block_lock);
    g_cond_init(&audio_out->block_cond);

    /* FreeRDP's to free(), in rdpsnd_server_context_free(). */
    server_formats = calloc(1, sizeof(*server_formats));
    if (server_formats)
        *server_formats = audio_out_format;

    rdpsnd->data = audio_out;
    rdpsnd->Activated = on_rdpsnd_activated;
    rdpsnd->ConfirmBlock = on_rdpsnd_confirm_block;
    rdpsnd->num_server_formats = server_formats ? 1 : 0;
    rdpsnd->server_formats = server_formats;
    rdpsnd->src_format = &audio_out_format;
    rdpsnd->use_dynamic_virtual_channel = g_strcmp0(g_getenv(DISABLE_DVC_ENV), "1") != 0;

    /* Initialize() also starts the channel. */
    if (!server_formats || rdpsnd->Initialize(rdpsnd, TRUE) != CHANNEL_RC_OK) {
        g_warning("rdpsnd Initialize failed");
        rdpsnd_server_context_free(rdpsnd);
        g_cond_clear(&audio_out->block_cond);
        g_mutex_clear(&audio_out->block_lock);
        g_mutex_clear(&audio_out->thread_lock);
        g_object_unref(audio_out->cancellable);
        g_free(audio_out);
        return NULL;
    }

    g_message("playback channel ready");

    return audio_out;
}

/* Ends the playback thread, if there is one. */
static void
audio_out_join(struct audio_out *audio_out)
{
    GThread *thread;

    g_mutex_lock(&audio_out->thread_lock);
    thread = g_steal_pointer(&audio_out->thread);
    g_mutex_unlock(&audio_out->thread_lock);

    if (thread)
        g_thread_join(thread);
}

void
audio_out_free(struct audio_out *audio_out)
{
    if (!audio_out)
        return;

    /* Ends a read or a wait between connection attempts; the broadcast is
     * for a thread waiting for a slot. */
    g_cancellable_cancel(audio_out->cancellable);
    g_mutex_lock(&audio_out->block_lock);
    g_cond_broadcast(&audio_out->block_cond);
    g_mutex_unlock(&audio_out->block_lock);

    /* Before rdpsnd is stopped: the thread calls SendSamples(). */
    audio_out_join(audio_out);

    audio_out->rdpsnd->Close(audio_out->rdpsnd);
    /* Joins rdpsnd's thread, so no activation runs after it. */
    audio_out->rdpsnd->Stop(audio_out->rdpsnd);

    /* An activation in between does not start a thread any more, but one
     * that was already past that check has. It leaves at once. */
    audio_out_join(audio_out);

    rdpsnd_server_context_free(audio_out->rdpsnd);

    g_free(audio_out->buffer);
    g_cond_clear(&audio_out->block_cond);
    g_mutex_clear(&audio_out->block_lock);
    g_mutex_clear(&audio_out->thread_lock);
    g_object_unref(audio_out->cancellable);
    g_free(audio_out);
}

/* ------------------------------------------------------------------ */
/* Capture: audin -> PipeWire protocol-simple                          */
/* ------------------------------------------------------------------ */

/* A thread keeps a connection to the capture server up, and on_audin_data(),
 * on the thread that audin's Open() starts, writes the client's samples to
 * it. Hence the lock around the connection.
 *
 * The connection is held for as long as the peer is there, not only while
 * samples flow: protocol-simple creates its stream per connected client, so
 * the connection is what makes the microphone exist in PipeWire's graph.
 * Dropping it between utterances would make the device come and go in every
 * application's device list. */
struct audio_in {
    audin_server_context *audin;

    GCancellable *cancellable;
    GThread *thread;

    GMutex source_lock;
    GSocketConnection *source;
};

static AUDIO_FORMAT audio_in_format = { WAVE_FORMAT_PCM, 1, 44100, 88200, 2, 16, 0, NULL };

static UINT
on_audin_receive_version(audin_server_context *context, const SNDIN_VERSION *version)
{
    return CHANNEL_RC_OK;
}

/* The client said which capture formats it has; ask it to open ours. We only
 * advertise the one, so there is nothing to choose, only to check that the
 * client offered it. */
static UINT
on_audin_receive_formats(audin_server_context *context, const SNDIN_FORMATS *formats)
{
    SNDIN_OPEN request = { 0 };
    UINT32 i;

    for (i = 0; i < formats->NumFormats; i++) {
        if (same_format(&formats->SoundFormats[i], &audio_in_format))
            break;
    }

    if (i == formats->NumFormats) {
        g_warning("client offered no matching capture format");
        return CHANNEL_RC_OK;
    }

    request.FramesPerPacket = audio_in_format.nSamplesPerSec / 100;
    request.initialFormat = i;
    request.captureFormat = audio_in_format;

    if (context->SendOpen(context, &request) != CHANNEL_RC_OK)
        g_warning("SendOpen failed");

    return CHANNEL_RC_OK;
}

static UINT
on_audin_open_reply(audin_server_context *context, const SNDIN_OPEN_REPLY *open_reply)
{
    if (open_reply->Result != 0)
        g_warning("client rejected capture Open (0x%x)", open_reply->Result);
    else
        g_message("the client's microphone is open");

    return CHANNEL_RC_OK;
}

static UINT
on_audin_incoming_data(audin_server_context *context, const SNDIN_DATA_INCOMING *data_incoming)
{
    return CHANNEL_RC_OK;
}

static UINT
on_audin_data(audin_server_context *context, const SNDIN_DATA *data)
{
    struct audio_in *audio_in = context->userdata;
    /* Data is positioned after the PDU header FreeRDP has consumed, so the
     * payload is what remains. */
    const void *samples = Stream_ConstPointer(data->Data);
    size_t bytes = Stream_GetRemainingLength(data->Data);
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&audio_in->source_lock);

    if (audio_in->source && bytes > 0) {
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(audio_in->source));
        g_autoptr(GError) error = NULL;

        /* Cancellable: a PipeWire that stopped reading would otherwise hold
         * this thread, and the lock, through teardown. */
        if (!g_output_stream_write_all(out, samples, bytes, NULL, audio_in->cancellable,
                                       &error)) {
            /* Drop it and let the capture thread connect again; a PipeWire
             * that restarted is the usual reason to get here. */
            if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                g_warning("capture send failed: %s", error->message);
            g_clear_object(&audio_in->source);
        }
    }

    return CHANNEL_RC_OK;
}

static UINT
on_audin_receive_format_change(audin_server_context *context,
                               const SNDIN_FORMATCHANGE *format_change)
{
    return CHANNEL_RC_OK;
}

static gpointer
audio_in_thread(gpointer data)
{
    struct audio_in *audio_in = data;

    while (!g_cancellable_is_cancelled(audio_in->cancellable)) {
        bool connected;

        g_mutex_lock(&audio_in->source_lock);
        connected = audio_in->source != NULL;
        g_mutex_unlock(&audio_in->source_lock);

        if (!connected) {
            GSocketConnection *source = connect_retrying(SOURCE_ADDR_ENV, SOURCE_ADDR_DEFAULT,
                                                         audio_in->cancellable, "capture");

            if (!source)
                break;

            g_mutex_lock(&audio_in->source_lock);
            audio_in->source = source;
            g_mutex_unlock(&audio_in->source_lock);
        }

        /* A tick rather than a watch on the socket: on_audin_data() may drop
         * the connection at any point. */
        if (!wait_unless_cancelled(audio_in->cancellable, POLL_INTERVAL_MS))
            break;
    }

    return NULL;
}

struct audio_in *
audio_in_new(HANDLE vcm)
{
    struct audio_in *audio_in;
    audin_server_context *audin;
    g_autoptr(GError) error = NULL;

    if (!vcm || vcm == INVALID_HANDLE_VALUE)
        return NULL;

    audin = audin_server_context_new(vcm);
    if (!audin) {
        g_warning("audin_server_context_new failed");
        return NULL;
    }

    audio_in = g_new0(struct audio_in, 1);
    audio_in->audin = audin;
    audio_in->cancellable = g_cancellable_new();
    g_mutex_init(&audio_in->source_lock);

    audin->userdata = audio_in;
    audin->serverVersion = SNDIN_VERSION_Version_2;
    audin->ReceiveVersion = on_audin_receive_version;
    audin->ReceiveFormats = on_audin_receive_formats;
    audin->OpenReply = on_audin_open_reply;
    audin->IncomingData = on_audin_incoming_data;
    audin->Data = on_audin_data;
    audin->ReceiveFormatChange = on_audin_receive_format_change;

    if (!audin_server_set_formats(audin, 1, &audio_in_format) || !audin->Open(audin)) {
        g_warning("cannot open the capture channel");
        audin_server_context_free(audin);
        g_mutex_clear(&audio_in->source_lock);
        g_object_unref(audio_in->cancellable);
        g_free(audio_in);
        return NULL;
    }

    audio_in->thread = g_thread_try_new("audio-in", audio_in_thread, audio_in, &error);
    if (!audio_in->thread)
        g_warning("cannot start the capture thread: %s", error->message);

    g_message("capture channel ready");

    return audio_in;
}

void
audio_in_free(struct audio_in *audio_in)
{
    if (!audio_in)
        return;

    g_cancellable_cancel(audio_in->cancellable);
    if (audio_in->thread)
        g_thread_join(audio_in->thread);

    /* After Close() no Data callback can race us for the connection. */
    audio_in->audin->Close(audio_in->audin);
    audin_server_context_free(audio_in->audin);

    g_clear_object(&audio_in->source);
    g_mutex_clear(&audio_in->source_lock);
    g_object_unref(audio_in->cancellable);
    g_free(audio_in);
}
