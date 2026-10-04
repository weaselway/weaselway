/*
 * See rdp.h. Ported from mutter's src/backends/rdp/meta-rdp-server.c; the
 * comments there say more about why the FreeRDP calls are in the order they
 * are in.
 *
 * Not ported yet: the client's scale factor, and the error frame -- a client
 * that cannot do gfxredir is disconnected, with the reason in the log.
 */

#define G_LOG_DOMAIN "rdp"

#include "rdp.h"
#include "audio.h"
#include "clipboard.h"
#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <linux/input-event-codes.h>
#include <linux/vm_sockets.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <freerdp/freerdp.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/disp.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/input.h>
#include <freerdp/listener.h>
#include <freerdp/locale/keyboard.h>
#include <freerdp/peer.h>
#include <freerdp/server/disp.h>
#include <freerdp/server/drdynvc.h>
#include <freerdp/server/gfxredir.h>
#include <freerdp/server/rdpei.h>
#include <freerdp/channels/rdpei.h>
#include <freerdp/update.h>
#include <freerdp/version.h>
#include <winpr/input.h>
#include <winpr/synch.h>
#include <winpr/wtsapi.h>

/* An upper bound on the number of FreeRDP event handles, listener or peer. */
#define MAX_FREERDP_FDS 32

/* The one fullscreen desktop window, RDP_RAIL_DESKTOP_WINDOW_ID. */
#define DESKTOP_WINDOW_ID 0xFFFFFFFF
#define POOL_ID 1
/* Buffer ids are 1-based. */
#define BUFFER_ID(i) ((uint64_t)((i) + 1))

/* Three buffers: one the client is reading, one a readback is landing in, and
 * one free to start the next frame on. */
#define N_BUFFERS 3

/* A client with gfxredir confirms caps right after the channel opens. One
 * without it, or without the share to map, never answers. */
#define GFXREDIR_CAPS_TIMEOUT_MS 5000
#define DRDYNVC_TIMEOUT_MS 30000
#define DRDYNVC_POLL_MS 10

/* How long the screen gets to take the size the client asked for, before the
 * client is resized to the screen instead. */
#define SIZE_REQUEST_TIMEOUT_MS 3000

/*
 * One buffer in the pool. @stale is what it is missing relative to the most
 * recently written one: only the damaged part of a frame is read back, so a
 * buffer that sat out a frame has a hole where that frame's damage went. It is
 * filled from the up to date buffer before the next write, so every buffer is
 * whole -- the client's full refresh shows all of it.
 *
 * A bounding box rather than a region: it over-copies when the damage is in
 * two far corners, and is one struct.
 */
struct buffer {
    size_t offset;       /* inside the pool */
    bool in_flight;      /* presented, not acked yet: the client's to read */
    uint64_t present_id;
    bool is_stale;
    struct rdp_rect stale;
};

/* FreeRDP allocates this inside the peer (ContextSize), so it has to begin
 * with the rdpContext. */
struct peer_context {
    rdpContext rdp_context;

    struct rdp_server *server;
    freerdp_peer *peer;
    HANDLE vcm;
    /* Held around WTSVirtualChannelManagerCheckFileDescriptorEx(), which is
     * what writes queued channel data to the socket. The playback thread
     * calls it too, so that sound does not wait for the main loop. Around the
     * whole drain: the queue is shared by all channels, and two threads
     * taking from it at once could send two chunks of one PDU out of order. */
    GMutex vcm_drain_lock;

    bool activated;
    /* Between our DesktopResize and the client's re-activation its surface
     * is the old size, and nothing may be presented. */
    bool resize_pending;
    /* gfxredir turned out to be unusable; the peer is dropped at the end of
     * the dispatch. */
    bool failed;

    DrdynvcServerContext *drdynvc;
    bool drdynvc_waiting;
    gint64 drdynvc_wait_start;

    GfxRedirServerContext *gfxredir;
    int gfxredir_activated;        /* atomic */
    gint64 gfxredir_caps_deadline; /* 0 for none */

    /* MS-RDPEDISP: the client says what size its window has. */
    DispServerContext *disp;

    /* rdpsnd and audin; see audio.h. */
    struct audio_out *audio_out;
    struct audio_in *audio_in;

    /* cliprdr; see clipboard.h. */
    struct clipboard *clipboard;

    /* MS-RDPEI: the fingers on the client's touchpad. */
    RdpeiServerContext *rdpei;
    /* When the last touch frame came, while fingers are down; 0 otherwise.
     * Under gfxredir_lock. */
    gint64 touch_last;

    /* The gfxredir and disp callbacks run on the channels' own reader
     * threads. They only record what happened here, under this lock, and wake
     * the main loop. */
    GMutex gfxredir_lock;
    bool disp_requested;
    int disp_width, disp_height;
    bool gfxredir_present_requested;
    const char *gfxredir_caps_error;
    uint64_t gfxredir_acked[N_BUFFERS];
    int gfxredir_n_acked;
    /* presentIds are never reused. When the pool is rebuilt this is the
     * highest one issued against the old pool, so a late ack cannot retire
     * the same-numbered buffer of the new one. */
    uint64_t present_id_floor;

    bool pool_created;
    int pool_width, pool_height, pool_stride;
    struct buffer buffers[N_BUFFERS];
    int next_buffer;   /* round-robin cursor */
    int last_written;  /* the buffer that is up to date, -1 if none */
    int writing;       /* the buffer of the frame in progress, -1 if none */
    int n_presents_inflight;
    uint64_t current_frame_id;

    int shm_fd;
    void *shm_addr;
    size_t shm_size;
    /* A GUID in braces, the name of the section the client opens, and the
     * file on the share that is. */
    char *shm_name;
    char *shm_path;
};

/* A descriptor of FreeRDP's that the main loop watches for us. */
struct watch {
    int fd;
    gpointer tag;
    bool peer;
};

struct rdp_source {
    GSource source;
    struct rdp_server *server;
};

struct rdp_server {
    struct rdp_config config;

    freerdp_listener *listener;
    int owned_listen_fd;
    char *cert_pem;
    char *key_pem;

    /* One client at a time. */
    struct peer_context *peer;

    /* Our place in the main loop: FreeRDP's descriptors, the deadlines
     * below, and @woken, which the channels' threads set to get it out of
     * poll(). */
    GMainContext *context;
    GSource *source;
    GArray *watches;
    gint64 deadline; /* -1 for none */
    int woken;       /* atomic */

    int screen_width, screen_height;
    bool full_requested;

    /* The pointer's image, bottom-up as the wire wants it; NULL for a hidden
     * pointer. Nothing is sent before the first rdp_server_set_pointer(). */
    bool pointer_set;
    uint8_t *pointer;
    int pointer_width, pointer_height;
    int pointer_hot_x, pointer_hot_y;

    /* The size the client asked for, and until when the screen may take to
     * get there. */
    int wanted_width, wanted_height;
    gint64 wanted_deadline;
    bool size_request_pending;
    /* Bumped whenever a pool goes away, which ends any frame begun on it. */
    uint64_t generation;
};

#define MS(ms) ((gint64)(ms) * G_TIME_SPAN_MILLISECOND)

/* For the channels' threads: have the main loop dispatch us. */
static void
wake(struct rdp_server *server)
{
    g_atomic_int_set(&server->woken, 1);
    g_main_context_wakeup(server->context);
}

static void
rect_union(struct rdp_rect *into, const struct rdp_rect *rect)
{
    int x1 = MIN(into->x, rect->x);
    int y1 = MIN(into->y, rect->y);
    int x2 = MAX(into->x + into->width, rect->x + rect->width);
    int y2 = MAX(into->y + into->height, rect->y + rect->height);

    *into = (struct rdp_rect){ x1, y1, x2 - x1, y2 - y1 };
}

static bool
rect_contains(const struct rdp_rect *outer, const struct rdp_rect *inner)
{
    return inner->x >= outer->x && inner->y >= outer->y &&
           inner->x + inner->width <= outer->x + outer->width &&
           inner->y + inner->height <= outer->y + outer->height;
}

static void
buffer_add_stale(struct buffer *buffer, const struct rdp_rect *rect)
{
    if (buffer->is_stale) {
        rect_union(&buffer->stale, rect);
    } else {
        buffer->stale = *rect;
        buffer->is_stale = true;
    }
}

static void
peer_fail(struct peer_context *peer_ctx, const char *reason)
{
    if (peer_ctx->failed)
        return;

    g_warning("no shared-memory graphics for this client, dropping it: %s", reason);
    peer_ctx->failed = true;
}

/* ------------------------------------------------------------------ */
/* The shared-memory pool                                             */
/* ------------------------------------------------------------------ */

static void
free_shared_memory(struct peer_context *peer_ctx)
{
    if (peer_ctx->shm_addr)
        munmap(peer_ctx->shm_addr, peer_ctx->shm_size);
    peer_ctx->shm_addr = NULL;
    peer_ctx->shm_size = 0;

    g_clear_fd(&peer_ctx->shm_fd, NULL);
    if (peer_ctx->shm_path)
        g_unlink(peer_ctx->shm_path);
    g_clear_pointer(&peer_ctx->shm_path, g_free);
    g_clear_pointer(&peer_ctx->shm_name, g_free);
}

/* A GUID-named file on the share; the client opens the section of that name. */
static bool
allocate_shared_memory(struct peer_context *peer_ctx, size_t size)
{
    g_autofree char *uuid = g_uuid_string_random();
    g_autofree char *name = g_strdup_printf("{%s}", uuid);
    g_autofree char *path = g_build_filename(peer_ctx->server->config.shm_dir, name, NULL);
    g_autofd int fd = g_open(path, O_CREAT | O_RDWR | O_EXCL | O_CLOEXEC, S_IWUSR | S_IRUSR);
    void *addr;

    if (fd < 0) {
        g_warning("cannot create %s: %s", path, g_strerror(errno));
        return false;
    }

    if (fallocate(fd, 0, 0, (off_t)size) < 0) {
        g_warning("cannot allocate %zu bytes of shared memory: %s", size, g_strerror(errno));
        goto fail;
    }

    addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        g_warning("cannot map %zu bytes of shared memory: %s", size, g_strerror(errno));
        goto fail;
    }

    peer_ctx->shm_fd = g_steal_fd(&fd);
    peer_ctx->shm_addr = addr;
    peer_ctx->shm_size = size;
    peer_ctx->shm_name = g_steal_pointer(&name);
    peer_ctx->shm_path = g_steal_pointer(&path);
    g_message("allocated shared memory %s (%zu bytes)", peer_ctx->shm_name, size);
    return true;

fail:
    /* Nothing else knows the name, so it would stay on the share for good. */
    g_unlink(path);
    return false;
}

static void
destroy_pool(struct peer_context *peer_ctx)
{
    GfxRedirServerContext *redir = peer_ctx->gfxredir;

    if (!peer_ctx->pool_created)
        return;

    /* A frame being written is about to lose the memory under it. */
    peer_ctx->server->generation++;

    if (redir) {
        GFXREDIR_CLOSE_POOL_PDU close_pool = { .poolId = POOL_ID };

        for (int i = 0; i < N_BUFFERS; i++) {
            GFXREDIR_DESTROY_BUFFER_PDU destroy_buffer = { .bufferId = BUFFER_ID(i) };

            redir->DestroyBuffer(redir, &destroy_buffer);
        }
        redir->ClosePool(redir, &close_pool);
    }

    free_shared_memory(peer_ctx);

    /* Every present so far named a buffer that no longer exists. Their acks
     * may still arrive, or never; refuse to retire anything up to here. */
    g_mutex_lock(&peer_ctx->gfxredir_lock);
    peer_ctx->present_id_floor = peer_ctx->current_frame_id;
    peer_ctx->gfxredir_n_acked = 0;
    g_mutex_unlock(&peer_ctx->gfxredir_lock);

    memset(peer_ctx->buffers, 0, sizeof(peer_ctx->buffers));
    peer_ctx->pool_created = false;
    peer_ctx->next_buffer = 0;
    peer_ctx->last_written = -1;
    peer_ctx->writing = -1;
    peer_ctx->n_presents_inflight = 0;
}

/* Create the pool and its buffers at the screen's size, once and on resize. */
static bool
ensure_pool(struct peer_context *peer_ctx, int width, int height)
{
    GfxRedirServerContext *redir = peer_ctx->gfxredir;
    int stride = width * 4;
    size_t size = (size_t)stride * (size_t)height;
    /* Buffers are a whole number of pages apart: the pool is a mapped file,
     * so its length has to be page aligned (fallocate on the share rejects
     * anything else), and it keeps the row copies between two buffers equally
     * aligned on both sides. The client is told the offsets. */
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    size_t buffer_pitch = (size + page_size - 1) & ~(page_size - 1);
    size_t pool_size = buffer_pitch * N_BUFFERS;
    g_autofree gunichar2 *section_name = NULL;
    glong section_name_length = 0;
    GFXREDIR_OPEN_POOL_PDU open_pool = { 0 };

    if (width <= 0 || height <= 0)
        return false;

    if (peer_ctx->pool_created && peer_ctx->pool_width == width && peer_ctx->pool_height == height)
        return true;

    destroy_pool(peer_ctx);

    if (!allocate_shared_memory(peer_ctx, pool_size))
        return false;

    /* The section name is UTF-16 on the wire, counted with its terminator. */
    section_name = g_utf8_to_utf16(peer_ctx->shm_name, -1, NULL, &section_name_length, NULL);

    open_pool.poolId = POOL_ID;
    open_pool.poolSize = pool_size;
    open_pool.sectionNameLength = (UINT32)section_name_length + 1;
    open_pool.sectionName = section_name;
    if (!section_name || redir->OpenPool(redir, &open_pool) != CHANNEL_RC_OK) {
        g_warning("gfxredir OpenPool failed");
        free_shared_memory(peer_ctx);
        return false;
    }

    for (int i = 0; i < N_BUFFERS; i++) {
        GFXREDIR_CREATE_BUFFER_PDU create_buffer = {
            .poolId = POOL_ID,
            .bufferId = BUFFER_ID(i),
            .offset = (size_t)i * buffer_pitch,
            .stride = (UINT32)stride,
            .width = (UINT32)width,
            .height = (UINT32)height,
            .format = GFXREDIR_BUFFER_PIXEL_FORMAT_ARGB_8888,
        };

        if (redir->CreateBuffer(redir, &create_buffer) != CHANNEL_RC_OK) {
            GFXREDIR_CLOSE_POOL_PDU close_pool = { .poolId = POOL_ID };

            g_warning("gfxredir CreateBuffer failed");
            redir->ClosePool(redir, &close_pool);
            free_shared_memory(peer_ctx);
            return false;
        }

        peer_ctx->buffers[i] = (struct buffer){ .offset = (size_t)i * buffer_pitch };
    }

    peer_ctx->pool_created = true;
    peer_ctx->pool_width = width;
    peer_ctx->pool_height = height;
    peer_ctx->pool_stride = stride;
    peer_ctx->next_buffer = 0;
    /* Nothing has been written, so there is nothing to copy from either; the
     * first frame is a full one. */
    peer_ctx->last_written = -1;
    peer_ctx->writing = -1;
    peer_ctx->n_presents_inflight = 0;
    g_message("gfxredir pool created: %d buffers of %dx%d, %zu bytes", N_BUFFERS, width, height,
            pool_size);
    return true;
}

static void
copy_between_buffers(struct peer_context *peer_ctx, int src_index, int dst_index,
                     const struct rdp_rect *rect)
{
    uint8_t *base = peer_ctx->shm_addr;
    const uint8_t *src = base + peer_ctx->buffers[src_index].offset;
    uint8_t *dst = base + peer_ctx->buffers[dst_index].offset;
    size_t stride = (size_t)peer_ctx->pool_stride;
    size_t row_bytes = (size_t)rect->width * 4;

    for (int y = rect->y; y < rect->y + rect->height; y++) {
        size_t row = (size_t)y * stride + (size_t)rect->x * 4;

        memcpy(dst + row, src + row, row_bytes);
    }
}

/*
 * A buffer the client is not reading, or -1.
 *
 * Among the free ones the buffer that is missing the least wins, and the
 * round-robin cursor only breaks ties. The most recently written one is
 * missing nothing, and it is free again as soon as the client has acked it, so
 * a client that acks within a frame keeps getting the same buffer and nothing
 * is copied between buffers at all.
 */
static int
acquire_buffer(struct peer_context *peer_ctx)
{
    int best = -1;
    int64_t best_area = 0;

    for (int n = 0; n < N_BUFFERS; n++) {
        int i = (peer_ctx->next_buffer + n) % N_BUFFERS;
        const struct buffer *buffer = &peer_ctx->buffers[i];
        int64_t area;

        if (buffer->in_flight)
            continue;

        if (i == peer_ctx->last_written) {
            best = i;
            break;
        }

        area = buffer->is_stale ? (int64_t)buffer->stale.width * buffer->stale.height : 0;
        if (best < 0 || area < best_area) {
            best = i;
            best_area = area;
        }
    }

    if (best >= 0)
        peer_ctx->next_buffer = (best + 1) % N_BUFFERS;
    return best;
}

static struct peer_context *
frame_peer(struct rdp_server *server, const struct rdp_frame *frame)
{
    struct peer_context *peer_ctx = server->peer;

    if (!peer_ctx || frame->generation != server->generation || !peer_ctx->pool_created ||
        frame->index < 0 || frame->index != peer_ctx->writing)
        return NULL;
    return peer_ctx;
}

/*
 * Write queued channel data to the socket now, rather than when poll() next
 * notices the channel manager's event.
 *
 * Also called on the playback thread (audio.c). What is below
 * CheckFileDescriptorEx() can be reached from another thread: the message
 * queue has its own lock, every PDU gets a stream of its own, and the
 * transport serialises writes. Legacy RDP encryption, with its RC4 state and
 * sequence counter, would not be safe, but only TLS is ever enabled here.
 * Opening drdynvc is the main loop's business, hence autoOpen FALSE.
 */
static void
flush_channels(struct peer_context *peer_ctx)
{
    if (!peer_ctx->vcm)
        return;

    g_mutex_lock(&peer_ctx->vcm_drain_lock);
    (void)WTSVirtualChannelManagerCheckFileDescriptorEx(peer_ctx->vcm, FALSE);
    g_mutex_unlock(&peer_ctx->vcm_drain_lock);
}

static void
audio_flush(void *data)
{
    flush_channels(data);
}

bool
rdp_server_begin_frame(struct rdp_server *server, const struct rdp_rect *rect,
                       struct rdp_frame *frame)
{
    struct peer_context *peer_ctx = server->peer;
    struct buffer *buffer;
    int index;

    if (rdp_server_state(server) != RDP_READY)
        return false;

    if (!ensure_pool(peer_ctx, server->screen_width, server->screen_height)) {
        peer_fail(peer_ctx, "The shared-memory pool could not be created.");
        return false;
    }

    index = acquire_buffer(peer_ctx);
    if (index < 0)
        return false;
    buffer = &peer_ctx->buffers[index];

    /* Bring the buffer up to date before writing into it: it missed every
     * frame that went to another one. Nothing to do where @rect covers the
     * hole, which in steady state it nearly always does -- the damage lands
     * in much the same place every frame. */
    if (buffer->is_stale && peer_ctx->last_written >= 0 && peer_ctx->last_written != index &&
        !rect_contains(rect, &buffer->stale))
        copy_between_buffers(peer_ctx, peer_ctx->last_written, index, &buffer->stale);
    buffer->is_stale = false;

    peer_ctx->writing = index;
    frame->generation = server->generation;
    frame->index = index;
    return true;
}

uint8_t *
rdp_server_frame_pixels(struct rdp_server *server, const struct rdp_frame *frame)
{
    struct peer_context *peer_ctx = frame_peer(server, frame);

    if (!peer_ctx)
        return NULL;
    return (uint8_t *)peer_ctx->shm_addr + peer_ctx->buffers[frame->index].offset;
}

void
rdp_server_cancel_frame(struct rdp_server *server, const struct rdp_frame *frame)
{
    struct peer_context *peer_ctx = frame_peer(server, frame);

    if (!peer_ctx)
        return;

    /* The buffer was brought up to date and then not written, which leaves it
     * exactly as up to date as the last written one. */
    peer_ctx->writing = -1;
}

/* Publish a buffer whose pixels are in shared memory. Any earlier and the
 * client would upload pixels that are not there yet, and @last_written would
 * name a buffer with a hole in it, which the stale copies would then spread
 * to the others. */
void
rdp_server_end_frame(struct rdp_server *server, const struct rdp_frame *frame,
                     const struct rdp_rect *rect)
{
    struct peer_context *peer_ctx = frame_peer(server, frame);
    GFXREDIR_PRESENT_BUFFER_PDU present = { 0 };
    RECTANGLE_32 opaque_rect;
    struct buffer *buffer;

    if (!peer_ctx)
        return;

    buffer = &peer_ctx->buffers[frame->index];
    peer_ctx->writing = -1;

    /* This buffer is now current; every other one is missing this frame. */
    peer_ctx->last_written = frame->index;
    for (int i = 0; i < N_BUFFERS; i++) {
        if (i != frame->index)
            buffer_add_stale(&peer_ctx->buffers[i], rect);
    }

    opaque_rect.left = (UINT32)rect->x;
    opaque_rect.top = (UINT32)rect->y;
    opaque_rect.width = (UINT32)rect->width;
    opaque_rect.height = (UINT32)rect->height;

    present.timestamp = 0; /* no A/V sync on the client */
    present.presentId = ++peer_ctx->current_frame_id;
    present.windowId = DESKTOP_WINDOW_ID;
    present.bufferId = BUFFER_ID(frame->index);
    present.orientation = 0;
    present.targetWidth = (UINT32)peer_ctx->pool_width;
    present.targetHeight = (UINT32)peer_ctx->pool_height;
    present.dirtyRect = opaque_rect;
    present.numOpaqueRects = 1;
    present.opaqueRects = &opaque_rect;

    if (peer_ctx->gfxredir->PresentBuffer(peer_ctx->gfxredir, &present) != CHANNEL_RC_OK) {
        g_warning("gfxredir PresentBuffer failed");
        return;
    }

    buffer->in_flight = true;
    buffer->present_id = present.presentId;
    peer_ctx->n_presents_inflight++;
    flush_channels(peer_ctx);

    if (server->config.verbose)
        g_debug("present %llu: buffer %d, %dx%d+%d+%d, %d in flight",
                (unsigned long long)present.presentId, frame->index, rect->width, rect->height,
                rect->x, rect->y, peer_ctx->n_presents_inflight);
}

/* ------------------------------------------------------------------ */
/* gfxredir                                                           */
/* ------------------------------------------------------------------ */

/* Channel thread. */
static void
gfxredir_reject_caps(struct peer_context *peer_ctx, const char *reason)
{
    g_mutex_lock(&peer_ctx->gfxredir_lock);
    peer_ctx->gfxredir_caps_error = reason;
    g_mutex_unlock(&peer_ctx->gfxredir_lock);
    wake(peer_ctx->server);
}

/* Channel thread. */
static UINT
gfxredir_legacy_caps(GfxRedirServerContext *context, const GFXREDIR_LEGACY_CAPS_PDU *caps)
{
    gfxredir_reject_caps(context->custom, "The client only speaks gfxredir v1; v2 is required.");
    return CHANNEL_RC_OK;
}

/* Channel thread. */
static UINT
gfxredir_caps_advertise(GfxRedirServerContext *context,
                        const GFXREDIR_CAPS_ADVERTISE_PDU *advertise)
{
    struct peer_context *peer_ctx = context->custom;
    const GFXREDIR_CAPS_HEADER *current = (const GFXREDIR_CAPS_HEADER *)advertise->caps;
    const GFXREDIR_CAPS_HEADER *selected = NULL;
    uint32_t selected_version = 0;
    uint32_t length = advertise->length;
    GFXREDIR_CAPS_CONFIRM_PDU confirm = { 0 };

    while (length >= sizeof(GFXREDIR_CAPS_HEADER)) {
        if (current->signature != GFXREDIR_CAPS_SIGNATURE)
            return ERROR_INVALID_DATA;
        /* A zero length would loop here forever. */
        if (current->length < sizeof(GFXREDIR_CAPS_HEADER) || current->length > length)
            return ERROR_INVALID_DATA;
        if (current->version >= selected_version) {
            selected = current;
            selected_version = current->version;
        }
        length -= current->length;
        current = (const GFXREDIR_CAPS_HEADER *)((const BYTE *)current + current->length);
    }

    if (!selected || selected_version < GFXREDIR_CAPS_VERSION2_0) {
        gfxredir_reject_caps(peer_ctx, "The client offered no gfxredir v2 caps.");
        return CHANNEL_RC_OK;
    }

    confirm.version = selected->version;
    confirm.length = selected->length;
    confirm.capsData = (const BYTE *)(selected + 1);
    context->GraphicsRedirectionCapsConfirm(context, &confirm);

    g_atomic_int_set(&peer_ctx->gfxredir_activated, 1);
    g_message("gfxredir activated (caps v0x%x)", (unsigned)selected->version);

    /* The client's screen is empty; fill it without waiting for damage. */
    g_mutex_lock(&peer_ctx->gfxredir_lock);
    peer_ctx->gfxredir_present_requested = true;
    g_mutex_unlock(&peer_ctx->gfxredir_lock);
    wake(peer_ctx->server);
    return CHANNEL_RC_OK;
}

/* Channel thread. */
static UINT
gfxredir_present_buffer_ack(GfxRedirServerContext *context,
                            const GFXREDIR_PRESENT_BUFFER_ACK_PDU *ack)
{
    struct peer_context *peer_ctx = context->custom;

    if (ack->windowId != DESKTOP_WINDOW_ID)
        return CHANNEL_RC_OK;

    g_mutex_lock(&peer_ctx->gfxredir_lock);
    /* An ack at or below the floor is for a pool that is gone. We never have
     * more presents outstanding than buffers, so there is always room. */
    if (ack->presentId > peer_ctx->present_id_floor && peer_ctx->gfxredir_n_acked < N_BUFFERS)
        peer_ctx->gfxredir_acked[peer_ctx->gfxredir_n_acked++] = ack->presentId;
    g_mutex_unlock(&peer_ctx->gfxredir_lock);
    wake(peer_ctx->server);
    return CHANNEL_RC_OK;
}

/* The main-loop half of the three callbacks above. */
static void
gfxredir_dispatch(struct peer_context *peer_ctx)
{
    uint64_t acked[N_BUFFERS];
    const char *caps_error;
    bool present_requested;
    int n_acked;

    g_mutex_lock(&peer_ctx->gfxredir_lock);
    caps_error = peer_ctx->gfxredir_caps_error;
    peer_ctx->gfxredir_caps_error = NULL;
    present_requested = peer_ctx->gfxredir_present_requested;
    peer_ctx->gfxredir_present_requested = false;
    n_acked = peer_ctx->gfxredir_n_acked;
    memcpy(acked, peer_ctx->gfxredir_acked, sizeof(acked));
    peer_ctx->gfxredir_n_acked = 0;
    g_mutex_unlock(&peer_ctx->gfxredir_lock);

    if (caps_error) {
        peer_fail(peer_ctx, caps_error);
        return;
    }

    /* The acked buffers are ours to write again. */
    for (int a = 0; a < n_acked; a++) {
        for (int i = 0; i < N_BUFFERS; i++) {
            struct buffer *buffer = &peer_ctx->buffers[i];

            if (buffer->in_flight && buffer->present_id == acked[a]) {
                buffer->in_flight = false;
                peer_ctx->n_presents_inflight--;
                break;
            }
        }
    }

    if (present_requested) {
        peer_ctx->gfxredir_caps_deadline = 0;
        peer_ctx->server->full_requested = true;
    }

    if (peer_ctx->gfxredir_caps_deadline &&
        g_get_monotonic_time() > peer_ctx->gfxredir_caps_deadline &&
        !g_atomic_int_get(&peer_ctx->gfxredir_activated)) {
        peer_ctx->gfxredir_caps_deadline = 0;
        peer_fail(peer_ctx, "The client did not open the gfxredir channel. It has to be "
                            "weaselway's sdl-freerdp, started with /wslgsharedmemorypath.");
    }
}

static void
setup_gfxredir(struct peer_context *peer_ctx)
{
    GfxRedirServerContext *redir = gfxredir_server_context_new(peer_ctx->vcm);

    if (!redir) {
        peer_fail(peer_ctx, "The gfxredir channel could not be created.");
        return;
    }

    redir->custom = peer_ctx;
    redir->GraphicsRedirectionLegacyCaps = gfxredir_legacy_caps;
    redir->GraphicsRedirectionCapsAdvertise = gfxredir_caps_advertise;
    redir->PresentBufferAck = gfxredir_present_buffer_ack;

    if (redir->Open(redir) != CHANNEL_RC_OK) {
        gfxredir_server_context_free(redir);
        peer_fail(peer_ctx, "The gfxredir channel could not be opened.");
        return;
    }

    peer_ctx->gfxredir = redir;
    peer_ctx->gfxredir_caps_deadline = g_get_monotonic_time() + MS(GFXREDIR_CAPS_TIMEOUT_MS);
    g_message("gfxredir channel opened, waiting for the client's caps");
}

/* ------------------------------------------------------------------ */
/* The client's size                                                  */
/* ------------------------------------------------------------------ */

static void
request_size(struct rdp_server *server, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;

    g_message("the client wants %dx%d", width, height);
    server->wanted_width = width;
    server->wanted_height = height;
    server->wanted_deadline = g_get_monotonic_time() + MS(SIZE_REQUEST_TIMEOUT_MS);
    server->size_request_pending = true;
}

/* Channel thread. */
static UINT
disp_monitor_layout(DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *pdu)
{
    struct peer_context *peer_ctx = context->custom;
    const DISPLAY_CONTROL_MONITOR_LAYOUT *primary = NULL;

    if (pdu->NumMonitors == 0)
        return CHANNEL_RC_OK;

    /* One screen: the primary monitor, or the first if none is flagged. */
    for (UINT32 i = 0; i < pdu->NumMonitors; i++) {
        if (pdu->Monitors[i].Flags & DISPLAY_CONTROL_MONITOR_PRIMARY) {
            primary = &pdu->Monitors[i];
            break;
        }
    }
    if (!primary)
        primary = &pdu->Monitors[0];

    g_mutex_lock(&peer_ctx->gfxredir_lock);
    peer_ctx->disp_requested = true;
    peer_ctx->disp_width = (int)primary->Width;
    peer_ctx->disp_height = (int)primary->Height;
    g_mutex_unlock(&peer_ctx->gfxredir_lock);
    wake(peer_ctx->server);
    return CHANNEL_RC_OK;
}

/* The main-loop half. */
static void
disp_dispatch(struct peer_context *peer_ctx)
{
    bool requested;
    int width, height;

    g_mutex_lock(&peer_ctx->gfxredir_lock);
    requested = peer_ctx->disp_requested;
    peer_ctx->disp_requested = false;
    width = peer_ctx->disp_width;
    height = peer_ctx->disp_height;
    g_mutex_unlock(&peer_ctx->gfxredir_lock);

    /* No DesktopResize from here: that goes out once the screen has the new
     * size, see peer_sync_desktop_size(). */
    if (requested)
        request_size(peer_ctx->server, width, height);
}

/* Without the channel the size is whatever the client connected with. */
static void
setup_disp(struct peer_context *peer_ctx)
{
    DispServerContext *disp = disp_server_context_new(peer_ctx->vcm);

    if (!disp)
        return;

    disp->custom = peer_ctx;
    disp->MaxNumMonitors = 1;
    disp->MaxMonitorAreaFactorA = DISPLAY_CONTROL_MAX_MONITOR_WIDTH;
    disp->MaxMonitorAreaFactorB = DISPLAY_CONTROL_MAX_MONITOR_HEIGHT;
    disp->DispMonitorLayout = disp_monitor_layout;

    if (disp->Open(disp) != CHANNEL_RC_OK) {
        disp_server_context_free(disp);
        return;
    }
    if (disp->DisplayControlCaps(disp) != CHANNEL_RC_OK) {
        disp->Close(disp);
        disp_server_context_free(disp);
        return;
    }
    peer_ctx->disp = disp;
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

static void
pointer_position(struct peer_context *peer_ctx, UINT16 x, UINT16 y)
{
    struct rdp_server *server = peer_ctx->server;

    input_pointer_motion(server->config.input, x, y, server->screen_width, server->screen_height);
}

static BOOL
on_mouse_event(rdpInput *rdp_input, UINT16 flags, UINT16 x, UINT16 y)
{
    struct peer_context *peer_ctx = (struct peer_context *)rdp_input->context;
    struct input *input = peer_ctx->server->config.input;
    uint16_t button = 0;

    if (!peer_ctx->activated || !input)
        return TRUE;

    if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) {
        /* Nine bits, two's complement, in 1/120ths of a notch. If both are
         * set the vertical wheel wins. */
        int value = flags & 0xff;

        if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
            value -= 0x100;
        input_pointer_wheel(input, value, !(flags & PTR_FLAGS_WHEEL));
        return TRUE;
    }

    pointer_position(peer_ctx, x, y);

    if (flags & PTR_FLAGS_BUTTON1)
        button = BTN_LEFT;
    else if (flags & PTR_FLAGS_BUTTON2)
        button = BTN_RIGHT;
    else if (flags & PTR_FLAGS_BUTTON3)
        button = BTN_MIDDLE;
    if (button)
        input_pointer_button(input, button, !!(flags & PTR_FLAGS_DOWN));
    return TRUE;
}

static BOOL
on_extended_mouse_event(rdpInput *rdp_input, UINT16 flags, UINT16 x, UINT16 y)
{
    struct peer_context *peer_ctx = (struct peer_context *)rdp_input->context;
    struct input *input = peer_ctx->server->config.input;
    uint16_t button = 0;

    if (!peer_ctx->activated || !input)
        return TRUE;

    pointer_position(peer_ctx, x, y);

    if (flags & PTR_XFLAGS_BUTTON1)
        button = BTN_SIDE;
    else if (flags & PTR_XFLAGS_BUTTON2)
        button = BTN_EXTRA;
    if (button)
        input_pointer_button(input, button, !!(flags & PTR_XFLAGS_DOWN));
    return TRUE;
}

static BOOL
on_keyboard_event(rdpInput *rdp_input, UINT16 flags, UINT8 code)
{
    struct peer_context *peer_ctx = (struct peer_context *)rdp_input->context;
    struct input *input = peer_ctx->server->config.input;
    rdpSettings *settings = rdp_input->context->settings;
    uint32_t keyboard_type = freerdp_settings_get_uint32(settings, FreeRDP_KeyboardType);
    uint32_t full_code = code, vk_code, xkb_code;

    if (!peer_ctx->activated || !input)
        return TRUE;

    if (flags & KBD_FLAGS_EXTENDED)
        full_code |= KBD_FLAGS_EXTENDED;

    vk_code = GetVirtualKeyCodeFromVirtualScanCode(full_code, keyboard_type);
    if (flags & KBD_FLAGS_EXTENDED)
        vk_code |= KBDEXT;

    /* An xkb keycode is the evdev one plus 8; 0 is a key WinPR cannot map. */
    xkb_code = GetKeycodeFromVirtualKeyCode(vk_code, WINPR_KEYCODE_TYPE_XKB);
    if (xkb_code < 8)
        return TRUE;

    /* In FreeRDP 3 KBD_FLAGS_DOWN marks a repeat; a press is the absence of
     * KBD_FLAGS_RELEASE. */
    input_key(input, (uint16_t)(xkb_code - 8), !(flags & KBD_FLAGS_RELEASE));
    return TRUE;
}

static BOOL
on_unicode_keyboard_event(rdpInput *rdp_input, UINT16 flags, UINT16 code)
{
    return TRUE;
}

static BOOL
on_synchronize_event(rdpInput *rdp_input, UINT32 flags)
{
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* The peer                                                           */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Touch (MS-RDPEI)                                                   */
/* ------------------------------------------------------------------ */

/*
 * The client sends the contacts on its touchpad once three or more fingers
 * are down; with fewer, Windows makes pointer motion and scrolling of them,
 * which arrive as mouse events. They go to the uinput touchpad as they are,
 * and it is libinput in the compositor that makes swipes and pinches of them.
 *
 * A contact's position is the finger's place on the pad, mapped onto the
 * desktop: a fraction of the desktop's size is that fraction of the pad's.
 */

/* Fingers the client has said nothing about for this long are lifted. Its own
 * watchdog lifts them sooner; this is for the lift that got lost. */
#define TOUCH_TIMEOUT_MS 500

/* On the rdpei channel's reader thread. */
static UINT
rdpei_touch_event(RdpeiServerContext *context, const RDPINPUT_TOUCH_EVENT *event)
{
    struct peer_context *peer_ctx = context->user_data;
    struct input *input = peer_ctx->server->config.input;
    rdpSettings *settings = peer_ctx->peer->context->settings;
    double width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    double height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);

    if (!input || width < 2 || height < 2)
        return CHANNEL_RC_OK;

    for (UINT16 f = 0; f < event->frameCount; f++) {
        const RDPINPUT_TOUCH_FRAME *frame = &event->frames[f];

        for (UINT32 c = 0; c < frame->contactCount; c++) {
            const RDPINPUT_CONTACT_DATA *contact = &frame->contacts[c];
            bool up = contact->contactFlags &
                      (RDPINPUT_CONTACT_FLAG_UP | RDPINPUT_CONTACT_FLAG_CANCELED);

            if (!up &&
                !(contact->contactFlags & (RDPINPUT_CONTACT_FLAG_DOWN | RDPINPUT_CONTACT_FLAG_UPDATE)))
                continue;
            if (peer_ctx->server->config.verbose)
                g_debug("touch: contact %u %s at %d,%d", (unsigned)contact->contactId,
                        up ? "up" : "down", (int)contact->x, (int)contact->y);
            input_touchpad_contact(input, contact->contactId, !up, contact->x / (width - 1),
                                   contact->y / (height - 1));
        }
        /* One frame is one instant; an event can carry several. */
        input_touchpad_frame(input);
    }

    g_mutex_lock(&peer_ctx->gfxredir_lock);
    peer_ctx->touch_last = g_get_monotonic_time();
    g_mutex_unlock(&peer_ctx->gfxredir_lock);
    wake(peer_ctx->server);
    return CHANNEL_RC_OK;
}

static UINT
rdpei_client_ready(RdpeiServerContext *context)
{
    g_message("touch: the client is ready (version 0x%08x, %u touch points)",
            (unsigned)context->clientVersion, (unsigned)context->maxTouchPoints);
    return CHANNEL_RC_OK;
}

/* Once the channel is open, which is the first moment the server's half of
 * the handshake can be sent. On the rdpei thread. */
static BOOL
rdpei_channel_id_assigned(RdpeiServerContext *context, UINT32 channel_id)
{
    (void)channel_id;
    return rdpei_server_send_sc_ready(context, RDPINPUT_PROTOCOL_V300, 0) == CHANNEL_RC_OK;
}

static void
setup_rdpei(struct peer_context *peer_ctx)
{
    RdpeiServerContext *rdpei;

    if (!peer_ctx->server->config.input)
        return;

    rdpei = rdpei_server_context_new(peer_ctx->vcm);
    if (!rdpei)
        return;

    rdpei->user_data = peer_ctx;
    rdpei->onChannelIdAssigned = rdpei_channel_id_assigned;
    rdpei->onClientReady = rdpei_client_ready;
    rdpei->onTouchEvent = rdpei_touch_event;

    if (rdpei->Open(rdpei) != CHANNEL_RC_OK) {
        g_warning("cannot open the touch channel; no touchpad gestures");
        rdpei_server_context_free(rdpei);
        return;
    }
    peer_ctx->rdpei = rdpei;
}

/* When fingers that nothing is heard of are to be lifted; -1 if none are
 * down. Lifts them when it is time. */
static gint64
touch_deadline(struct peer_context *peer_ctx)
{
    gint64 last;
    bool expired;

    g_mutex_lock(&peer_ctx->gfxredir_lock);
    last = peer_ctx->touch_last;
    expired = last && g_get_monotonic_time() - last >= MS(TOUCH_TIMEOUT_MS);
    if (expired)
        peer_ctx->touch_last = 0;
    g_mutex_unlock(&peer_ctx->gfxredir_lock);

    if (!last)
        return -1;
    if (!expired)
        return last + MS(TOUCH_TIMEOUT_MS);
    /* Nothing is held when all fingers were lifted properly. */
    input_touchpad_release(peer_ctx->server->config.input);
    return -1;
}

/* ------------------------------------------------------------------ */
/* The pointer                                                        */
/* ------------------------------------------------------------------ */

/* MS-RDPBCGR 2.2.9.1.2.1.11: a Large Pointer Update tops out at 384x384. */
#define MAX_POINTER_SIZE 384

static void
peer_send_pointer(struct peer_context *peer_ctx)
{
    struct rdp_server *server = peer_ctx->server;
    rdpUpdate *update = peer_ctx->peer->context->update;

    if (!peer_ctx->activated || peer_ctx->failed || !server->pointer_set)
        return;

    update->BeginPaint(update->context);
    if (server->pointer) {
        POINTER_LARGE_UPDATE pointer = { 0 };

        pointer.xorBpp = 32;
        pointer.cacheIndex = 0;
        pointer.hotSpotX = (UINT16)server->pointer_hot_x;
        pointer.hotSpotY = (UINT16)server->pointer_hot_y;
        pointer.width = (UINT16)server->pointer_width;
        pointer.height = (UINT16)server->pointer_height;
        /* A 32bpp xorMask carries its own alpha, so no separate AND mask. */
        pointer.lengthAndMask = 0;
        pointer.andMaskData = NULL;
        pointer.lengthXorMask = (UINT32)server->pointer_width * 4 * (UINT32)server->pointer_height;
        pointer.xorMaskData = server->pointer;
        update->pointer->PointerLarge(update->context, &pointer);
    } else {
        POINTER_SYSTEM_UPDATE pointer = { .type = SYSPTR_NULL };

        update->pointer->PointerSystem(update->context, &pointer);
    }
    update->EndPaint(update->context);
}

void
rdp_server_set_pointer(struct rdp_server *server, const uint8_t *pixels, int stride,
                       int width, int height, int hot_x, int hot_y)
{
    g_clear_pointer(&server->pointer, g_free);
    server->pointer_set = true;

    if (pixels && (width <= 0 || height <= 0 || width > MAX_POINTER_SIZE ||
                   height > MAX_POINTER_SIZE)) {
        g_warning("a %dx%d pointer cannot be sent; hiding it", width, height);
        pixels = NULL;
    }
    if (pixels) {
        server->pointer = g_malloc((size_t)width * 4 * (size_t)height);

        /* Pointer bitmaps are bottom-up, like a Windows DIB. B G R A in
         * memory is what the wire calls ARGB. */
        for (int y = 0; y < height; y++)
            memcpy(server->pointer + (size_t)(height - 1 - y) * (size_t)width * 4,
                   pixels + (size_t)y * (size_t)stride, (size_t)width * 4);
        server->pointer_width = width;
        server->pointer_height = height;
        server->pointer_hot_x = CLAMP(hot_x, 0, width - 1);
        server->pointer_hot_y = CLAMP(hot_y, 0, height - 1);
    }

    if (server->peer)
        peer_send_pointer(server->peer);
}

static void
peer_destroy(struct peer_context *peer_ctx)
{
    struct rdp_server *server = peer_ctx->server;
    freerdp_peer *client = peer_ctx->peer;

    g_message("client disconnected");

    if (server->peer == peer_ctx)
        server->peer = NULL;
    if (server->config.input)
        input_release_all(server->config.input);

    client->Disconnect(client);
    freerdp_peer_context_free(client);
    freerdp_peer_free(client);
}

static BOOL
peer_finish_activation(freerdp_peer *client)
{
    struct peer_context *peer_ctx = (struct peer_context *)client->context;

    if (peer_ctx->activated) {
        /* The client came back after our DesktopResize. Its surface was
         * recreated at the new size and holds nothing. */
        if (peer_ctx->resize_pending) {
            peer_ctx->resize_pending = false;
            peer_ctx->server->full_requested = true;
            g_message("client re-activated at %ux%u",
                    freerdp_settings_get_uint32(client->context->settings, FreeRDP_DesktopWidth),
                    freerdp_settings_get_uint32(client->context->settings, FreeRDP_DesktopHeight));
        }
        peer_send_pointer(peer_ctx);
        return TRUE;
    }

    peer_ctx->activated = true;
    g_message("client activated");

    /* The client has no pointer shape until we send one. */
    peer_send_pointer(peer_ctx);

    /* The client dictates the size: the screen is asked to take the one it
     * connected with. */
    request_size(peer_ctx->server,
                 (int)freerdp_settings_get_uint32(client->context->settings, FreeRDP_DesktopWidth),
                 (int)freerdp_settings_get_uint32(client->context->settings, FreeRDP_DesktopHeight));

    /* Before gfxredir, as Weston does. The first frame goes out once the
     * client has confirmed gfxredir caps. */
    setup_disp(peer_ctx);
    /* Without sound if either fails. */
    peer_ctx->audio_out = audio_out_new(peer_ctx->vcm, audio_flush, peer_ctx,
                                        peer_ctx->server->config.verbose);
    peer_ctx->audio_in = audio_in_new(peer_ctx->vcm);
    /* Without a clipboard if the client did not ask for the channel. */
    peer_ctx->clipboard = clipboard_new(peer_ctx->vcm, client->context,
                                        peer_ctx->server->config.selection);
    setup_rdpei(peer_ctx);
    setup_gfxredir(peer_ctx);
    return TRUE;
}

/* While a peer waits for drdynvc to reach READY, pump it every
 * DRDYNVC_POLL_MS. Returns false if the peer has to go. */
static bool
peer_drdynvc_wait(struct peer_context *peer_ctx)
{
    freerdp_peer *client = peer_ctx->peer;
    BOOL alive;

    if (WTSVirtualChannelManagerGetDrdynvcState(peer_ctx->vcm) == DRDYNVC_STATE_READY) {
        peer_ctx->drdynvc_waiting = false;
        return peer_finish_activation(client);
    }

    if (g_get_monotonic_time() - peer_ctx->drdynvc_wait_start > MS(DRDYNVC_TIMEOUT_MS)) {
        g_warning("drdynvc did not become ready");
        return false;
    }

    if (!client->CheckFileDescriptor(client))
        return false;

    /* FreeRDP 3 asserts if this runs before the client has joined drdynvc. */
    if (!WTSVirtualChannelManagerIsChannelJoined(peer_ctx->vcm, DRDYNVC_SVC_CHANNEL_NAME))
        return true;

    g_mutex_lock(&peer_ctx->vcm_drain_lock);
    alive = WTSVirtualChannelManagerCheckFileDescriptor(peer_ctx->vcm);
    g_mutex_unlock(&peer_ctx->vcm_drain_lock);
    return alive;
}

static BOOL
on_peer_activate(freerdp_peer *client)
{
    struct peer_context *peer_ctx = (struct peer_context *)client->context;
    rdpSettings *settings = client->context->settings;

    g_message("client activating at %ux%u",
            freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
            freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight));

    if (!peer_ctx->vcm)
        return FALSE;

    /* gfxredir is a dynamic virtual channel, so drdynvc has to be up first. */
    if (!peer_ctx->drdynvc) {
        DrdynvcServerContext *drdynvc = drdynvc_server_context_new(peer_ctx->vcm);

        if (!drdynvc)
            return FALSE;
        if (drdynvc->Start(drdynvc) != CHANNEL_RC_OK) {
            drdynvc_server_context_free(drdynvc);
            return FALSE;
        }
        peer_ctx->drdynvc = drdynvc;
    }

    if (WTSVirtualChannelManagerGetDrdynvcState(peer_ctx->vcm) != DRDYNVC_STATE_READY) {
        /* The channel manager only opens drdynvc for an activated client. */
        client->activated = TRUE;
        if (!peer_ctx->drdynvc_waiting) {
            peer_ctx->drdynvc_waiting = true;
            peer_ctx->drdynvc_wait_start = g_get_monotonic_time();
        }
        return TRUE;
    }

    return peer_finish_activation(client);
}

static BOOL
on_peer_capabilities(freerdp_peer *client)
{
    return TRUE;
}

static BOOL
on_peer_post_connect(freerdp_peer *client)
{
    return TRUE;
}

/* Pump the peer. Returns false if it has to go. */
static bool
peer_activity(struct peer_context *peer_ctx)
{
    freerdp_peer *client = peer_ctx->peer;
    BOOL auto_open;
    BOOL alive;

    if (!client->CheckFileDescriptor(client))
        return false;

    if (!peer_ctx->vcm)
        return true;

    /* The channel manager has to be pumped unconditionally: its event stays
     * signalled until it is drained. Opening drdynvc is what is conditional
     * -- FreeRDP 3 would otherwise push DVC caps while the client is still
     * waiting for its license PDU. */
    auto_open = client->activated &&
                WTSVirtualChannelManagerIsChannelJoined(peer_ctx->vcm, DRDYNVC_SVC_CHANNEL_NAME);
    g_mutex_lock(&peer_ctx->vcm_drain_lock);
    alive = WTSVirtualChannelManagerCheckFileDescriptorEx(peer_ctx->vcm, auto_open);
    g_mutex_unlock(&peer_ctx->vcm_drain_lock);
    return alive;
}

/*
 * Get the client onto the screen's size. Returns true while it is not there
 * yet, in which case nothing may be presented: its surface and its buffer
 * mappings are the old size until it re-activates.
 */
static bool
peer_sync_desktop_size(struct peer_context *peer_ctx)
{
    struct rdp_server *server = peer_ctx->server;
    freerdp_peer *client = peer_ctx->peer;
    rdpSettings *settings = client->context->settings;
    int width = server->screen_width, height = server->screen_height;

    if (peer_ctx->resize_pending)
        return true;

    if ((int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) == width &&
        (int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight) == height)
        return false;

    /* The screen is still on its way to the size the client asked for. If
     * it is the client that is on that size already, this is all there is to
     * wait for; if not, the client follows once the screen is there. */
    if ((width != server->wanted_width || height != server->wanted_height) &&
        g_get_monotonic_time() < server->wanted_deadline)
        return true;

    if (!freerdp_settings_get_bool(settings, FreeRDP_DesktopResize)) {
        peer_fail(peer_ctx, "The client cannot be resized to the screen's size.");
        return true;
    }

    g_message("resizing the client's desktop to %dx%d", width, height);

    /* Now, while the channel is healthy: the client throws its gfxredir state
     * away on DesktopResize, unacked presents included, and those buffers
     * would stay in flight for good. */
    destroy_pool(peer_ctx);

    (void)freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, (UINT32)width);
    (void)freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, (UINT32)height);

    /* Deactivate-All, then the client activates again; the dynamic channels
     * survive it. */
    peer_ctx->resize_pending = true;
    client->context->update->DesktopResize(client->context);
    return true;
}

static BOOL
peer_context_new(freerdp_peer *client, rdpContext *context)
{
    struct peer_context *peer_ctx = (struct peer_context *)context;

    peer_ctx->peer = client;
    peer_ctx->shm_fd = -1;
    peer_ctx->last_written = -1;
    peer_ctx->writing = -1;
    g_mutex_init(&peer_ctx->gfxredir_lock);
    g_mutex_init(&peer_ctx->vcm_drain_lock);
    return TRUE;
}

static void
peer_context_free(freerdp_peer *client, rdpContext *context)
{
    struct peer_context *peer_ctx = (struct peer_context *)context;

    if (!peer_ctx)
        return;

    if (peer_ctx->server) {
        destroy_pool(peer_ctx);
        /* Also when no pool was ever made: a frame begun on this peer must
         * not match the next one. */
        peer_ctx->server->generation++;
    }

    /* These join the audio threads, of which the playback one flushes the
     * channels of this peer. */
    audio_out_free(peer_ctx->audio_out);
    peer_ctx->audio_out = NULL;
    audio_in_free(peer_ctx->audio_in);
    peer_ctx->audio_in = NULL;

    clipboard_free(peer_ctx->clipboard);
    peer_ctx->clipboard = NULL;

    if (peer_ctx->disp) {
        peer_ctx->disp->Close(peer_ctx->disp);
        disp_server_context_free(peer_ctx->disp);
        peer_ctx->disp = NULL;
    }

    if (peer_ctx->rdpei) {
        peer_ctx->rdpei->Close(peer_ctx->rdpei);
        rdpei_server_context_free(peer_ctx->rdpei);
        peer_ctx->rdpei = NULL;
    }

    if (peer_ctx->gfxredir) {
        /* Close() joins the channel's thread, so no callback runs after it. */
        peer_ctx->gfxredir->Close(peer_ctx->gfxredir);
        gfxredir_server_context_free(peer_ctx->gfxredir);
        peer_ctx->gfxredir = NULL;
    }
    g_mutex_clear(&peer_ctx->gfxredir_lock);

    if (peer_ctx->drdynvc) {
        peer_ctx->drdynvc->Stop(peer_ctx->drdynvc);
        drdynvc_server_context_free(peer_ctx->drdynvc);
        peer_ctx->drdynvc = NULL;
    }

    if (peer_ctx->vcm) {
        WTSCloseServer(peer_ctx->vcm);
        peer_ctx->vcm = NULL;
    }
    g_mutex_clear(&peer_ctx->vcm_drain_lock);
}

static bool
peer_init(freerdp_peer *client, struct rdp_server *server)
{
    struct peer_context *peer_ctx;
    rdpSettings *settings;
    rdpPrivateKey *key;
    rdpCertificate *cert;
    rdpInput *input;

    client->ContextSize = sizeof(struct peer_context);
    client->ContextNew = peer_context_new;
    client->ContextFree = peer_context_free;
    if (!freerdp_peer_context_new(client))
        return false;

    peer_ctx = (struct peer_context *)client->context;
    peer_ctx->server = server;
    settings = client->context->settings;

    /* TLS with the throwaway certificate and no NLA. Nothing verifies it --
     * the client runs with /cert:ignore and the transport is a Hyper-V socket
     * inside the machine -- but FreeRDP will not initialize a peer without
     * one. The settings take ownership of both objects. */
    key = freerdp_key_new_from_pem(server->key_pem);
    cert = freerdp_certificate_new_from_pem(server->cert_pem);
    if (!key || !cert) {
        freerdp_key_free(key);
        freerdp_certificate_free(cert);
        goto fail;
    }
    if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1)) {
        freerdp_key_free(key);
        freerdp_certificate_free(cert);
        goto fail;
    }
    if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1)) {
        freerdp_certificate_free(cert);
        goto fail;
    }
    (void)freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);

    if (!client->Initialize(client))
        goto fail;

    (void)freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
    (void)freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_PSEUDO_XSERVER);
    (void)freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
    (void)freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, TRUE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_NSCodec, FALSE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled, TRUE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_RemoteApplicationMode, FALSE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, TRUE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, TRUE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_FastPathInput, TRUE);
    /* FreeRDP 3 added two connect-time steps that need callbacks or a UDP
     * path we do not have; without them the state machine is the 2.x one. */
    (void)freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, FALSE);
    (void)freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, FALSE);
    (void)freerdp_settings_set_uint32(settings, FreeRDP_MultitransportFlags, 0);

    client->Capabilities = on_peer_capabilities;
    client->PostConnect = on_peer_post_connect;
    client->Activate = on_peer_activate;

    input = client->context->input;
    input->SynchronizeEvent = on_synchronize_event;
    input->MouseEvent = on_mouse_event;
    input->ExtendedMouseEvent = on_extended_mouse_event;
    input->KeyboardEvent = on_keyboard_event;
    input->UnicodeKeyboardEvent = on_unicode_keyboard_event;

    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
    peer_ctx->vcm = WTSOpenServerA((LPSTR)peer_ctx);
    if (!peer_ctx->vcm || peer_ctx->vcm == INVALID_HANDLE_VALUE) {
        peer_ctx->vcm = NULL;
        g_warning("cannot create the virtual channel manager");
        goto fail;
    }

    server->peer = peer_ctx;
    g_message("client connected");
    return true;

fail:
    g_warning("cannot set up the incoming client");
    client->Close(client);
    freerdp_peer_context_free(client);
    return false;
}

/* FreeRDP frees the peer when this returns FALSE. */
static BOOL
on_peer_accepted(freerdp_listener *listener, freerdp_peer *client)
{
    struct rdp_server *server = listener->param4;

    if (server->peer) {
        g_warning("refusing a second client while one is connected");
        return FALSE;
    }

    return peer_init(client, server);
}

/* ------------------------------------------------------------------ */
/* The listener                                                       */
/* ------------------------------------------------------------------ */

static char *
bio_to_string(BIO *bio)
{
    char *data = NULL;
    long length = BIO_get_mem_data(bio, &data);

    return length > 0 ? g_strndup(data, (gsize)length) : NULL;
}

/* A self-signed RSA-2048 certificate, made in memory and never written. */
static bool
generate_session_tls(struct rdp_server *server, GError **error)
{
    EVP_PKEY *pkey = EVP_RSA_gen(2048);
    X509 *x509 = X509_new();
    BIO *cert_bio = BIO_new(BIO_s_mem());
    BIO *key_bio = BIO_new(BIO_s_mem());
    X509_NAME *name;
    bool ok = false;

    if (!pkey || !x509 || !cert_bio || !key_bio)
        goto out;

    if (!X509_set_version(x509, X509_VERSION_3) ||
        !ASN1_INTEGER_set(X509_get_serialNumber(x509), 1) ||
        !X509_gmtime_adj(X509_getm_notBefore(x509), 0) ||
        !X509_gmtime_adj(X509_getm_notAfter(x509), 365L * 24 * 60 * 60) ||
        !X509_set_pubkey(x509, pkey))
        goto out;

    name = X509_get_subject_name(x509);
    if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    (const unsigned char *)"weaselwayd", -1, -1, 0) ||
        !X509_set_issuer_name(x509, name) || !X509_sign(x509, pkey, EVP_sha256()))
        goto out;

    if (!PEM_write_bio_X509(cert_bio, x509) ||
        !PEM_write_bio_PrivateKey(key_bio, pkey, NULL, NULL, 0, NULL, NULL))
        goto out;

    server->cert_pem = bio_to_string(cert_bio);
    server->key_pem = bio_to_string(key_bio);
    ok = server->cert_pem && server->key_pem;

out:
    if (!ok)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "generating the session's TLS certificate failed");
    BIO_free(key_bio);
    BIO_free(cert_bio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return ok;
}

static int
create_vsock_fd(int port, GError **error)
{
    struct sockaddr_vm addr = {
        .svm_family = AF_VSOCK,
        .svm_cid = VMADDR_CID_ANY,
        .svm_port = (unsigned)port,
    };
    const int buffer_size = 65536;
    g_autofd int fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "cannot create a vsock: %s",
                    g_strerror(errno));
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 1) < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot listen on vsock port %d: %s", port, g_strerror(errno));
        return -1;
    }
    return g_steal_fd(&fd);
}

static bool
is_mount_point(const char *path)
{
    g_autofree char *parent = g_build_filename(path, "..", NULL);
    GStatBuf st, parent_st;

    if (g_stat(path, &st) || !S_ISDIR(st.st_mode) || g_stat(parent, &parent_st))
        return false;
    return st.st_dev != parent_st.st_dev;
}

/* ------------------------------------------------------------------ */
/* The main loop's side                                               */
/* ------------------------------------------------------------------ */

static int
collect_fds(int *fds, bool *is_peer, int n, bool peer, HANDLE *handles, DWORD count)
{
    for (DWORD i = 0; i < count; i++) {
        int fd = GetEventFileDescriptor(handles[i]);

        if (fd < 0)
            continue;
        fds[n] = fd;
        is_peer[n++] = peer;
    }
    return n;
}

/* Have the main loop watch what FreeRDP wants watched right now. The set
 * changes with the peer, and while it connects. */
static void
update_watches(struct rdp_server *server)
{
    HANDLE handles[MAX_FREERDP_FDS + 1];
    int fds[2 * (MAX_FREERDP_FDS + 1)];
    bool is_peer[2 * (MAX_FREERDP_FDS + 1)];
    struct peer_context *peer_ctx = server->peer;
    bool changed;
    DWORD count;
    int n = 0;

    count = server->listener->GetEventHandles(server->listener, handles, MAX_FREERDP_FDS);
    n = collect_fds(fds, is_peer, n, false, handles, count);

    if (peer_ctx) {
        count = peer_ctx->peer->GetEventHandles(peer_ctx->peer, handles, MAX_FREERDP_FDS);
        if (peer_ctx->vcm)
            handles[count++] = WTSVirtualChannelManagerGetEventHandle(peer_ctx->vcm);
        n = collect_fds(fds, is_peer, n, true, handles, count);
    }

    changed = (guint)n != server->watches->len;
    for (int i = 0; i < n && !changed; i++) {
        const struct watch *watch = &g_array_index(server->watches, struct watch, i);

        changed = watch->fd != fds[i] || watch->peer != is_peer[i];
    }
    if (!changed)
        return;

    for (guint i = 0; i < server->watches->len; i++)
        g_source_remove_unix_fd(server->source, g_array_index(server->watches, struct watch, i).tag);
    g_array_set_size(server->watches, 0);

    for (int i = 0; i < n; i++) {
        struct watch watch = {
            .fd = fds[i],
            .tag = g_source_add_unix_fd(server->source, fds[i], G_IO_IN),
            .peer = is_peer[i],
        };

        g_array_append_val(server->watches, watch);
    }
}

static bool
any_ready(struct rdp_server *server, bool peer)
{
    for (guint i = 0; i < server->watches->len; i++) {
        const struct watch *watch = &g_array_index(server->watches, struct watch, i);

        if (watch->peer == peer && g_source_query_unix_fd(server->source, watch->tag))
            return true;
    }
    return false;
}

/* When to dispatch even if nothing happens on a descriptor; -1 for never. */
static gint64
next_deadline(struct rdp_server *server)
{
    struct peer_context *peer_ctx = server->peer;
    gint64 now = g_get_monotonic_time();

    if (!peer_ctx)
        return -1;
    if (peer_ctx->drdynvc_waiting)
        return now + MS(DRDYNVC_POLL_MS);
    if (peer_ctx->gfxredir_caps_deadline)
        return peer_ctx->gfxredir_caps_deadline;
    /* A screen that does not follow is noticed by the clock alone. */
    if (server->wanted_deadline > now)
        return server->wanted_deadline;
    return touch_deadline(peer_ctx);
}

static gboolean
rdp_source_prepare(GSource *source, gint *timeout)
{
    struct rdp_server *server = ((struct rdp_source *)source)->server;
    gint64 now;

    update_watches(server);
    server->deadline = next_deadline(server);

    if (g_atomic_int_get(&server->woken)) {
        *timeout = 0;
        return TRUE;
    }
    if (server->deadline < 0) {
        *timeout = -1;
        return FALSE;
    }

    now = g_get_monotonic_time();
    *timeout = server->deadline > now ? (gint)((server->deadline - now + 999) / 1000) : 0;
    return *timeout == 0;
}

static gboolean
rdp_source_check(GSource *source)
{
    struct rdp_server *server = ((struct rdp_source *)source)->server;

    return g_atomic_int_get(&server->woken) ||
           (server->deadline >= 0 && g_get_monotonic_time() >= server->deadline) ||
           any_ready(server, true) || any_ready(server, false);
}

static gboolean
rdp_source_dispatch(GSource *source, GSourceFunc callback, gpointer user_data)
{
    struct rdp_server *server = ((struct rdp_source *)source)->server;
    struct peer_context *peer_ctx = server->peer;
    bool listener_ready = any_ready(server, false);

    /* Before the notes are read: one left after this is dispatched again. */
    g_atomic_int_set(&server->woken, 0);

    /* The peer before the listener: a client that reconnects is refused for
     * as long as the connection it left behind has not been noticed. */
    if (peer_ctx) {
        bool alive = true;

        if (any_ready(server, true))
            alive = peer_activity(peer_ctx);
        if (alive && peer_ctx->drdynvc_waiting)
            alive = peer_drdynvc_wait(peer_ctx);
        if (alive) {
            disp_dispatch(peer_ctx);
            gfxredir_dispatch(peer_ctx);
            alive = !peer_ctx->failed;
        }
        if (!alive)
            peer_destroy(peer_ctx);
    }

    if (listener_ready && !server->listener->CheckFileDescriptor(server->listener))
        g_warning("accepting a client failed");

    /* The client's state may be another now: ready for a frame, asking for a
     * size, gone. */
    if (server->config.changed)
        server->config.changed(server->config.changed_data);
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs rdp_source_funcs = {
    .prepare = rdp_source_prepare,
    .check = rdp_source_check,
    .dispatch = rdp_source_dispatch,
};

struct rdp_server *
rdp_server_new(const struct rdp_config *config, GError **error)
{
    struct rdp_server *server = g_new0(struct rdp_server, 1);

    server->config = *config;
    server->owned_listen_fd = -1;
    server->deadline = -1;
    server->watches = g_array_new(FALSE, FALSE, sizeof(struct watch));
    server->context = g_main_context_ref_thread_default();

    /* In a plain directory the client would never see the pools. */
    if (!config->shm_dir || !is_mount_point(config->shm_dir)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED,
                    "the shared-memory share is not mounted at %s",
                    config->shm_dir ? config->shm_dir : "(nowhere)");
        goto fail;
    }

    if (!generate_session_tls(server, error))
        goto fail;

    server->listener = freerdp_listener_new();
    if (!server->listener) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot create a listener");
        goto fail;
    }
    server->listener->PeerAccepted = on_peer_accepted;
    server->listener->param4 = server;

    if (config->tcp_port > 0) {
        if (!server->listener->Open(server->listener, "127.0.0.1", (UINT16)config->tcp_port)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot listen on 127.0.0.1:%d",
                        config->tcp_port);
            goto fail;
        }
        g_message("listening on 127.0.0.1:%d, unauthenticated (FreeRDP %s)", config->tcp_port,
                  FREERDP_VERSION_FULL);
    } else {
        server->owned_listen_fd = create_vsock_fd(config->vsock_port, error);
        if (server->owned_listen_fd < 0)
            goto fail;
        if (!server->listener->OpenFromSocket(server->listener, server->owned_listen_fd)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "cannot listen on the vsock");
            goto fail;
        }
        g_message("listening on vsock port %d (FreeRDP %s), frames go to %s", config->vsock_port,
                  FREERDP_VERSION_FULL, config->shm_dir);
    }

    server->source = g_source_new(&rdp_source_funcs, sizeof(struct rdp_source));
    ((struct rdp_source *)server->source)->server = server;
    g_source_set_static_name(server->source, "rdp");
    g_source_attach(server->source, server->context);

    return server;

fail:
    rdp_server_free(server);
    return NULL;
}

void
rdp_server_free(struct rdp_server *server)
{
    if (!server)
        return;

    if (server->source) {
        g_source_destroy(server->source);
        g_source_unref(server->source);
    }

    if (server->peer)
        peer_destroy(server->peer);

    if (server->listener) {
        server->listener->Close(server->listener);
        freerdp_listener_free(server->listener);
    }
    g_clear_fd(&server->owned_listen_fd, NULL);
    g_array_unref(server->watches);
    g_main_context_unref(server->context);
    g_free(server->cert_pem);
    g_free(server->key_pem);
    g_free(server->pointer);
    g_free(server);
}

void
rdp_server_set_screen_size(struct rdp_server *server, int width, int height)
{
    server->screen_width = width;
    server->screen_height = height;
}

bool
rdp_server_client_size(struct rdp_server *server, int *width, int *height)
{
    struct peer_context *peer_ctx = server->peer;

    if (!peer_ctx || !peer_ctx->activated || peer_ctx->failed || server->wanted_width <= 0 ||
        server->wanted_height <= 0)
        return false;

    *width = server->wanted_width;
    *height = server->wanted_height;
    return true;
}

enum rdp_state
rdp_server_state(struct rdp_server *server)
{
    struct peer_context *peer_ctx = server->peer;

    if (!peer_ctx || !peer_ctx->activated || peer_ctx->failed)
        return RDP_NO_CLIENT;
    if (server->screen_width <= 0 || server->screen_height <= 0)
        return RDP_CONNECTING;
    if (!g_atomic_int_get(&peer_ctx->gfxredir_activated))
        return RDP_CONNECTING;
    if (peer_sync_desktop_size(peer_ctx))
        return RDP_CONNECTING;
    if (peer_ctx->writing >= 0 || peer_ctx->n_presents_inflight >= N_BUFFERS)
        return RDP_BUSY;
    return RDP_READY;
}

bool
rdp_server_take_size_request(struct rdp_server *server, int *width, int *height)
{
    bool requested = server->size_request_pending;

    server->size_request_pending = false;
    *width = server->wanted_width;
    *height = server->wanted_height;
    return requested;
}

bool
rdp_server_take_full_request(struct rdp_server *server)
{
    bool requested = server->full_requested;

    server->full_requested = false;
    return requested;
}
