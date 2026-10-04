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

#include "audio.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

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

static void
audio_log(const char *format, ...)
{
    va_list args;

    fputs("rdp: audio: ", stderr);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/* Shared: interruptible waiting and connecting                        */
/* ------------------------------------------------------------------ */

/* Waits up to @timeout_ms. False as soon as @exit_fd is signalled, so every
 * sleep in this file is also a check for teardown; a thread could otherwise
 * not be joined before its timeout happened to run out. */
static bool
wait_or_exit(int exit_fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = exit_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);

    if (ret < 0) {
        if (errno == EINTR)
            return true;
        audio_log("poll failed: %s", strerror(errno));
        return false;
    }

    return ret == 0;
}

/* Splits "host:port" at the last colon. */
static bool
split_addr(const char *spec, char **host, char **service)
{
    const char *colon = strrchr(spec, ':');

    if (!colon || colon == spec || !colon[1]) {
        audio_log("malformed address '%s', want host:port", spec);
        return false;
    }

    *host = strndup(spec, (size_t)(colon - spec));
    *service = strdup(colon + 1);
    if (!*host || !*service) {
        free(*host);
        free(*service);
        return false;
    }

    return true;
}

static int
connect_once(const char *host, const char *service)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *result = NULL;
    int fd = -1;
    int err;

    err = getaddrinfo(host, service, &hints, &result);
    if (err != 0) {
        audio_log("cannot resolve %s:%s: %s", host, service, gai_strerror(err));
        return -1;
    }

    for (struct addrinfo *ai = result; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0)
            continue;

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;

        err = errno;
        close(fd);
        errno = err;
        fd = -1;
    }

    err = errno;
    freeaddrinfo(result);

    if (fd >= 0) {
        /* Both directions move packets of a few milliseconds. Nagle would
         * collect them into bursts, for no gain on a loopback connection. */
        int one = 1;

        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0)
            audio_log("TCP_NODELAY failed: %s", strerror(errno));
    }

    errno = err;
    return fd;
}

/* Connects, trying again until it works or teardown is signalled; -1 only for
 * the latter, so a caller can take that as "leave the thread".
 *
 * PipeWire is a service of its own that may start after us, and it drops
 * every client stream when it restarts. */
static int
connect_retrying(const char *env_var, const char *default_addr, int exit_fd,
                 const atomic_bool *exit_signal, const char *what)
{
    const char *spec = getenv(env_var);
    char *host = NULL;
    char *service = NULL;
    bool warned = false;
    int fd = -1;

    if (!spec || !spec[0])
        spec = default_addr;

    if (!split_addr(spec, &host, &service))
        return -1;

    while (!atomic_load(exit_signal)) {
        fd = connect_once(host, service);
        if (fd >= 0) {
            if (warned)
                audio_log("%s connected to %s", what, spec);
            break;
        }

        /* Once, not once a second: a PipeWire that is not running yet is an
         * ordinary race at startup, and trying again is the handling. */
        if (!warned) {
            audio_log("%s cannot reach %s (%s), retrying", what, spec, strerror(errno));
            warned = true;
        }

        if (!wait_or_exit(exit_fd, RECONNECT_INTERVAL_MS))
            break;
    }

    free(host);
    free(service);
    return fd;
}

static void
signal_exit(int exit_fd)
{
    uint64_t one = 1;

    if (write(exit_fd, &one, sizeof(one)) != sizeof(one))
        audio_log("exit_fd write failed: %s", strerror(errno));
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

    atomic_bool exit_signal;
    /* Started by the first activation, on rdpsnd's thread. */
    atomic_bool thread_started;
    pthread_t thread;

    /* Our connection to the protocol-simple playback server. */
    atomic_int sink_fd;

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

    /* Written by the playback thread, read by rdpsnd's in the confirm. */
    pthread_mutex_t block_lock;
    struct block_info block_info[MAX_BLOCKS_IN_FLIGHT];

    /* A semaphore that bounds the blocks in flight, Weston's audioSem.
     * Non-blocking: who waits for it polls exit_fd along with it, so that
     * teardown can end a wait nothing else would. */
    int block_sem;
    int exit_fd;

    audio_flush_func flush;
    void *flush_data;

    /* For the verbose log: the packets of the sound that is playing. */
    bool playing;
    unsigned sent;
    atomic_uint confirmed;
};

static AUDIO_FORMAT audio_out_format = { WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0, NULL };

/* Gives one slot back. Also called on rdpsnd's thread. */
static bool
block_sem_release(struct audio_out *audio_out)
{
    uint64_t one = 1;

    if (write(audio_out->block_sem, &one, sizeof(one)) != sizeof(one)) {
        audio_log("block_sem write failed: %s", strerror(errno));
        return false;
    }

    return true;
}

/* Takes one slot, waiting if there is none. False if teardown woke us
 * instead; no slot was taken then. */
static bool
block_sem_acquire(struct audio_out *audio_out)
{
    for (;;) {
        uint64_t dummy;
        struct pollfd fds[2];
        int ret;

        if (read(audio_out->block_sem, &dummy, sizeof(dummy)) == sizeof(dummy))
            return true;

        if (errno != EAGAIN && errno != EINTR) {
            audio_log("block_sem read failed: %s", strerror(errno));
            return false;
        }

        fds[0] = (struct pollfd){ .fd = audio_out->block_sem, .events = POLLIN };
        fds[1] = (struct pollfd){ .fd = audio_out->exit_fd, .events = POLLIN };

        ret = poll(fds, 2, 1000);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            audio_log("block_sem poll failed: %s", strerror(errno));
            return false;
        }

        if (ret == 0) {
            /* The sink socket is not being drained while we sit here, so
             * PipeWire starts dropping what it cannot hand us. */
            audio_log("stalled waiting for the client to confirm audio blocks");
            continue;
        }

        if (fds[1].revents)
            return false;
    }
}

/* Blocks that were unconfirmed when a connection dropped would keep their
 * slots forever, so every new connection starts with the full credit. */
static void
block_sem_reset(struct audio_out *audio_out)
{
    uint64_t dummy;

    pthread_mutex_lock(&audio_out->block_lock);

    while (read(audio_out->block_sem, &dummy, sizeof(dummy)) == sizeof(dummy))
        ;

    for (int i = 0; i < audio_out->max_in_flight; i++) {
        if (!block_sem_release(audio_out))
            break;
    }

    memset(audio_out->block_info, 0, sizeof(audio_out->block_info));

    pthread_mutex_unlock(&audio_out->block_lock);
}

/* On rdpsnd's thread. */
static UINT
on_rdpsnd_confirm_block(RdpsndServerContext *context, BYTE confirm_block_num, UINT16 wtimestamp)
{
    struct audio_out *audio_out = context->data;
    struct block_info *info = &audio_out->block_info[confirm_block_num];
    UINT status = CHANNEL_RC_OK;

    pthread_mutex_lock(&audio_out->block_lock);

    if (!info->in_flight) {
        audio_log("spurious confirm for block %u", confirm_block_num);
    } else if (!info->slot_released) {
        /* Release on the first confirm. Clients differ in how many they
         * send -- mstsc sends two a block once a latency is advertised
         * (received, then rendered), others one -- and waiting for a second
         * that never comes empties the semaphore after 256 blocks and stops
         * playback for good. */
        info->slot_released = true;
        atomic_fetch_add(&audio_out->confirmed, 1);
        if (!block_sem_release(audio_out))
            status = ERROR_INTERNAL_ERROR;
    }

    pthread_mutex_unlock(&audio_out->block_lock);

    return status;
}

/* Drops the oldest audio queued in the sink socket beyond max_backlog_bytes,
 * in whole frames. See MAX_BUFFERED_MS. */
static void
skip_backlog(struct audio_out *audio_out, int sink_fd)
{
    size_t excess;
    int available = 0;

    if (ioctl(sink_fd, FIONREAD, &available) != 0 || available < 0 ||
        (size_t)available <= audio_out->max_backlog_bytes)
        return;

    excess = (size_t)available - audio_out->max_backlog_bytes;
    excess -= excess % (size_t)audio_out->bytes_per_frame;

    if (audio_out->verbose)
        audio_log("dropping %zu bytes (%zu ms) of backlog", excess,
                  excess / (size_t)audio_out->bytes_per_frame * 1000 /
                      audio_out_format.nSamplesPerSec);

    while (excess > 0) {
        size_t want = excess < audio_out->buffer_size ? excess : audio_out->buffer_size;
        ssize_t n = read(sink_fd, audio_out->buffer, want);

        if (n < 0 && errno == EINTR)
            continue;
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
forward_packet(struct audio_out *audio_out, int sink_fd)
{
    size_t chunk = audio_out->buffer_size;
    size_t got = 0;
    BYTE block_no;

    skip_backlog(audio_out, sink_fd);

    while (got < chunk) {
        ssize_t n = read(sink_fd, audio_out->buffer + got, chunk - got);

        if (n > 0) {
            got += (size_t)n;
            continue;
        }

        if (n == 0) {
            /* Also what teardown's shutdown() looks like. */
            if (!atomic_load(&audio_out->exit_signal))
                audio_log("playback server closed the connection");
            return false;
        }

        if (errno == EINTR)
            continue;

        /* Teardown shuts the socket down to end exactly this read. */
        if (!atomic_load(&audio_out->exit_signal))
            audio_log("read from playback server failed: %s", strerror(errno));
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
            audio_log("silence after %u packets, of which the client confirmed %u",
                      audio_out->sent, atomic_load(&audio_out->confirmed));
        audio_out->playing = false;
        return true;
    }

    if (!audio_out->playing) {
        audio_out->playing = true;
        audio_out->sent = 0;
        atomic_store(&audio_out->confirmed, 0);
    }

    /* Waits until an earlier block has been confirmed. */
    if (!block_sem_acquire(audio_out))
        return false;

    pthread_mutex_lock(&audio_out->block_lock);
    block_no = audio_out->rdpsnd->block_no;
    audio_out->block_info[block_no].in_flight = true;
    audio_out->block_info[block_no].slot_released = false;
    pthread_mutex_unlock(&audio_out->block_lock);

    /* Timestamp 0 turns A/V sync off at the client, as in Weston; there is
     * nothing meaningful to give it. */
    if (audio_out->rdpsnd->SendSamples(audio_out->rdpsnd, audio_out->buffer,
                                       (size_t)audio_out->frames_per_packet, 0) != CHANNEL_RC_OK) {
        audio_log("SendSamples failed");
        return false;
    }

    if (block_no == audio_out->rdpsnd->block_no) {
        /* Nothing was sent this time; the slot goes back. */
        pthread_mutex_lock(&audio_out->block_lock);
        audio_out->block_info[block_no].in_flight = false;
        audio_out->block_info[block_no].slot_released = true;
        pthread_mutex_unlock(&audio_out->block_lock);

        return block_sem_release(audio_out);
    }

    audio_out->sent++;

    /* SendSamples() only queues. Without this the bytes wait for the main
     * loop's next turn, which is the coupling to the frames this thread is
     * there to avoid. */
    if (audio_out->flush)
        audio_out->flush(audio_out->flush_data);

    return true;
}

static void *
audio_out_thread(void *data)
{
    struct audio_out *audio_out = data;

    while (!atomic_load(&audio_out->exit_signal)) {
        int fd = connect_retrying(SINK_ADDR_ENV, SINK_ADDR_DEFAULT, audio_out->exit_fd,
                                  &audio_out->exit_signal, "playback");

        if (fd < 0)
            break;

        atomic_store(&audio_out->sink_fd, fd);

        /* Credit does not carry over: what was outstanding when the last
         * connection dropped is never going to be confirmed. */
        block_sem_reset(audio_out);

        while (!atomic_load(&audio_out->exit_signal)) {
            if (!forward_packet(audio_out, fd))
                break;
        }

        /* Not closed here if teardown is about to shut it down: the number
         * could be someone else's descriptor by then. */
        if (atomic_exchange(&audio_out->sink_fd, -1) == fd)
            close(fd);
    }

    return NULL;
}

/* On rdpsnd's thread. */
static void
on_rdpsnd_activated(RdpsndServerContext *context)
{
    struct audio_out *audio_out = context->data;
    const char *latency_env = getenv(LATENCY_MS_ENV);
    const char *silence_env = getenv(SEND_SILENCE_ENV);
    const AUDIO_FORMAT *client_format = NULL;
    int latency_ms = LATENCY_MS_DEFAULT;
    UINT16 format = 0;

    /* Clients may activate rdpsnd more than once (reconnect, format change);
     * the thread is set up on the first. */
    if (atomic_load(&audio_out->thread_started) || atomic_load(&audio_out->exit_signal))
        return;

    for (UINT16 i = 0; i < context->num_client_formats; i++) {
        if (same_format(&context->client_formats[i], &audio_out_format)) {
            client_format = &context->client_formats[i];
            format = i;
            break;
        }
    }

    if (!client_format) {
        audio_log("client and server agreed on no playback format");
        return;
    }

    audio_out->bytes_per_frame = (client_format->wBitsPerSample / 8) * client_format->nChannels;

    if (latency_env && latency_env[0]) {
        char *end;
        long parsed = strtol(latency_env, &end, 10);

        if (*end == '\0' && parsed >= LATENCY_MS_MIN && parsed <= LATENCY_MS_MAX)
            latency_ms = (int)parsed;
        else
            audio_log("ignoring %s='%s', want %d-%d", LATENCY_MS_ENV, latency_env,
                      LATENCY_MS_MIN, LATENCY_MS_MAX);
    }

    /* rdpsnd gets the figure we packetise at, so that its out_frames is our
     * frames_per_packet and every SendSamples() emits one PDU. */
    context->latency = (UINT32)latency_ms;
    audio_out->frames_per_packet = (int)client_format->nSamplesPerSec * latency_ms / 1000;

    audio_out->send_silence = silence_env && strcmp(silence_env, "1") == 0;

    /* One RDP packet, which is all forward_packet() ever reads at a time. */
    audio_out->buffer_size =
        (size_t)audio_out->frames_per_packet * (size_t)audio_out->bytes_per_frame;
    audio_out->buffer = calloc(1, audio_out->buffer_size);
    if (!audio_out->buffer) {
        audio_log("out of memory");
        return;
    }

    audio_out->max_in_flight = MAX_BUFFERED_MS / latency_ms;
    if (audio_out->max_in_flight < 2)
        audio_out->max_in_flight = 2;
    audio_out->max_backlog_bytes = (size_t)client_format->nSamplesPerSec * MAX_BUFFERED_MS /
                                   1000 * (size_t)audio_out->bytes_per_frame;

    if (context->SelectFormat(context, format) != CHANNEL_RC_OK ||
        context->SetVolume(context, 0x7FFF, 0x7FFF) != CHANNEL_RC_OK)
        audio_log("cannot select the playback format");

    audio_log("playback %d ms/packet (%d frames, %.0f packets/s)%s", latency_ms,
              audio_out->frames_per_packet, 1000.0 / latency_ms,
              audio_out->send_silence ? ", forwarding silence" : "");

    if (pthread_create(&audio_out->thread, NULL, audio_out_thread, audio_out) != 0) {
        audio_log("cannot start the playback thread");
        free(audio_out->buffer);
        audio_out->buffer = NULL;
        return;
    }
    atomic_store(&audio_out->thread_started, true);
}

struct audio_out *
audio_out_new(HANDLE vcm, audio_flush_func flush, void *flush_data, bool verbose)
{
    const char *dvc_env = getenv(DISABLE_DVC_ENV);
    struct audio_out *audio_out;
    AUDIO_FORMAT *server_formats;

    if (!vcm || vcm == INVALID_HANDLE_VALUE)
        return NULL;

    audio_out = calloc(1, sizeof(*audio_out));
    /* Freed by FreeRDP in rdpsnd_server_context_free(). */
    server_formats = calloc(1, sizeof(*server_formats));
    if (!audio_out || !server_formats)
        goto err_alloc;

    atomic_init(&audio_out->sink_fd, -1);
    audio_out->verbose = verbose;
    audio_out->flush = flush;
    audio_out->flush_data = flush_data;
    pthread_mutex_init(&audio_out->block_lock, NULL);

    audio_out->block_sem = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC);
    audio_out->exit_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (audio_out->block_sem < 0 || audio_out->exit_fd < 0) {
        audio_log("eventfd failed: %s", strerror(errno));
        goto err_fds;
    }

    audio_out->rdpsnd = rdpsnd_server_context_new(vcm);
    if (!audio_out->rdpsnd) {
        audio_log("rdpsnd_server_context_new failed");
        goto err_fds;
    }

    *server_formats = audio_out_format;

    audio_out->rdpsnd->data = audio_out;
    audio_out->rdpsnd->Activated = on_rdpsnd_activated;
    audio_out->rdpsnd->ConfirmBlock = on_rdpsnd_confirm_block;
    audio_out->rdpsnd->num_server_formats = 1;
    audio_out->rdpsnd->server_formats = server_formats;
    audio_out->rdpsnd->src_format = &audio_out_format;
    audio_out->rdpsnd->use_dynamic_virtual_channel = !(dvc_env && strcmp(dvc_env, "1") == 0);

    /* Initialize() also starts the channel. */
    if (audio_out->rdpsnd->Initialize(audio_out->rdpsnd, TRUE) != CHANNEL_RC_OK) {
        audio_log("rdpsnd Initialize failed");
        /* Frees server_formats. */
        rdpsnd_server_context_free(audio_out->rdpsnd);
        server_formats = NULL;
        goto err_fds;
    }

    audio_log("playback channel ready");

    return audio_out;

err_fds:
    if (audio_out->block_sem >= 0)
        close(audio_out->block_sem);
    if (audio_out->exit_fd >= 0)
        close(audio_out->exit_fd);
    pthread_mutex_destroy(&audio_out->block_lock);
err_alloc:
    free(server_formats);
    free(audio_out);
    return NULL;
}

/* Ends the playback thread, if there is one. */
static void
audio_out_join(struct audio_out *audio_out)
{
    int sink_fd;

    if (!atomic_exchange(&audio_out->thread_started, false))
        return;

    /* Wakes a thread waiting in block_sem_acquire() or between connection
     * attempts; the shutdown is for one in read(). */
    signal_exit(audio_out->exit_fd);

    sink_fd = atomic_exchange(&audio_out->sink_fd, -1);
    if (sink_fd >= 0)
        shutdown(sink_fd, SHUT_RDWR);

    pthread_join(audio_out->thread, NULL);

    if (sink_fd >= 0)
        close(sink_fd);
}

void
audio_out_free(struct audio_out *audio_out)
{
    if (!audio_out)
        return;

    atomic_store(&audio_out->exit_signal, true);

    /* Before rdpsnd is stopped: the thread calls SendSamples(). */
    audio_out_join(audio_out);

    audio_out->rdpsnd->Close(audio_out->rdpsnd);
    /* Joins rdpsnd's thread, so no activation runs after it. */
    audio_out->rdpsnd->Stop(audio_out->rdpsnd);

    /* One that was started by an activation in between; it leaves at once. */
    audio_out_join(audio_out);

    rdpsnd_server_context_free(audio_out->rdpsnd);

    free(audio_out->buffer);
    close(audio_out->block_sem);
    close(audio_out->exit_fd);
    pthread_mutex_destroy(&audio_out->block_lock);
    free(audio_out);
}

/* ------------------------------------------------------------------ */
/* Capture: audin -> PipeWire protocol-simple                          */
/* ------------------------------------------------------------------ */

/* A thread keeps a connection to the capture server up, and on_audin_data(),
 * on the thread that audin's Open() starts, writes the client's samples to
 * it. Hence the lock around source_fd.
 *
 * The connection is held for as long as the peer is there, not only while
 * samples flow: protocol-simple creates its stream per connected client, so
 * the connection is what makes the microphone exist in PipeWire's graph.
 * Dropping it between utterances would make the device come and go in every
 * application's device list. */
struct audio_in {
    audin_server_context *audin;

    atomic_bool exit_signal;
    bool thread_started;
    pthread_t thread;
    int exit_fd;

    pthread_mutex_t source_fd_lock;
    int source_fd;
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
        audio_log("client offered no matching capture format");
        return CHANNEL_RC_OK;
    }

    request.FramesPerPacket = audio_in_format.nSamplesPerSec / 100;
    request.initialFormat = i;
    request.captureFormat = audio_in_format;

    if (context->SendOpen(context, &request) != CHANNEL_RC_OK)
        audio_log("SendOpen failed");

    return CHANNEL_RC_OK;
}

static UINT
on_audin_open_reply(audin_server_context *context, const SNDIN_OPEN_REPLY *open_reply)
{
    if (open_reply->Result != 0)
        audio_log("client rejected capture Open (0x%x)", open_reply->Result);
    else
        audio_log("the client's microphone is open");

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

    pthread_mutex_lock(&audio_in->source_fd_lock);
    if (audio_in->source_fd >= 0 && bytes > 0) {
        ssize_t sent = send(audio_in->source_fd, samples, bytes, MSG_NOSIGNAL);

        if (sent != (ssize_t)bytes) {
            /* Drop it and let the capture thread connect again; a PipeWire
             * that restarted is the usual reason to get here. */
            audio_log("capture send failed: %s", sent < 0 ? strerror(errno) : "short write");
            close(audio_in->source_fd);
            audio_in->source_fd = -1;
        }
    }
    pthread_mutex_unlock(&audio_in->source_fd_lock);

    return CHANNEL_RC_OK;
}

static UINT
on_audin_receive_format_change(audin_server_context *context,
                               const SNDIN_FORMATCHANGE *format_change)
{
    return CHANNEL_RC_OK;
}

static void *
audio_in_thread(void *data)
{
    struct audio_in *audio_in = data;

    while (!atomic_load(&audio_in->exit_signal)) {
        bool connected;

        pthread_mutex_lock(&audio_in->source_fd_lock);
        connected = audio_in->source_fd >= 0;
        pthread_mutex_unlock(&audio_in->source_fd_lock);

        if (!connected) {
            int fd = connect_retrying(SOURCE_ADDR_ENV, SOURCE_ADDR_DEFAULT, audio_in->exit_fd,
                                      &audio_in->exit_signal, "capture");

            if (fd < 0)
                break;

            pthread_mutex_lock(&audio_in->source_fd_lock);
            audio_in->source_fd = fd;
            pthread_mutex_unlock(&audio_in->source_fd_lock);
        }

        /* A tick rather than a poll of the socket: on_audin_data() may close
         * it at any point, and a descriptor polled here could by then be
         * closed and its number used again. */
        if (!wait_or_exit(audio_in->exit_fd, POLL_INTERVAL_MS))
            break;
    }

    return NULL;
}

struct audio_in *
audio_in_new(HANDLE vcm)
{
    struct audio_in *audio_in;

    if (!vcm || vcm == INVALID_HANDLE_VALUE)
        return NULL;

    audio_in = calloc(1, sizeof(*audio_in));
    if (!audio_in)
        return NULL;

    audio_in->source_fd = -1;
    pthread_mutex_init(&audio_in->source_fd_lock, NULL);

    audio_in->exit_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (audio_in->exit_fd < 0) {
        audio_log("eventfd failed: %s", strerror(errno));
        goto err_lock;
    }

    audio_in->audin = audin_server_context_new(vcm);
    if (!audio_in->audin) {
        audio_log("audin_server_context_new failed");
        goto err_fd;
    }

    if (!audin_server_set_formats(audio_in->audin, 1, &audio_in_format)) {
        audio_log("audin_server_set_formats failed");
        goto err_audin;
    }

    audio_in->audin->userdata = audio_in;
    audio_in->audin->serverVersion = SNDIN_VERSION_Version_2;
    audio_in->audin->ReceiveVersion = on_audin_receive_version;
    audio_in->audin->ReceiveFormats = on_audin_receive_formats;
    audio_in->audin->OpenReply = on_audin_open_reply;
    audio_in->audin->IncomingData = on_audin_incoming_data;
    audio_in->audin->Data = on_audin_data;
    audio_in->audin->ReceiveFormatChange = on_audin_receive_format_change;

    if (!audio_in->audin->Open(audio_in->audin)) {
        audio_log("audin Open failed");
        goto err_audin;
    }

    if (pthread_create(&audio_in->thread, NULL, audio_in_thread, audio_in) == 0)
        audio_in->thread_started = true;
    else
        audio_log("cannot start the capture thread");

    audio_log("capture channel ready");

    return audio_in;

err_audin:
    audin_server_context_free(audio_in->audin);
err_fd:
    close(audio_in->exit_fd);
err_lock:
    pthread_mutex_destroy(&audio_in->source_fd_lock);
    free(audio_in);
    return NULL;
}

void
audio_in_free(struct audio_in *audio_in)
{
    if (!audio_in)
        return;

    if (audio_in->thread_started) {
        atomic_store(&audio_in->exit_signal, true);
        signal_exit(audio_in->exit_fd);
        pthread_join(audio_in->thread, NULL);
    }

    /* After Close() no Data callback can race us for source_fd. */
    audio_in->audin->Close(audio_in->audin);
    audin_server_context_free(audio_in->audin);

    if (audio_in->source_fd >= 0)
        close(audio_in->source_fd);
    pthread_mutex_destroy(&audio_in->source_fd_lock);

    close(audio_in->exit_fd);
    free(audio_in);
}
