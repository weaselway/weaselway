/*
 * weaselwayd: the userspace half of dxgdrm's virtual display.
 *
 * The compositor scans out to dxgdrm's KMS node. This waits for its commits,
 * reads the damaged part of each frame back and hands it to the Windows client
 * through gfxredir shared memory (rdp.c); the client's mouse and keyboard come
 * back as uinput devices (input.c).
 *
 * The readback goes through GL rather than D3D12 directly: the frame is a
 * D3D12 shared handle, Mesa's d3d12 driver imports one as a dma-buf
 * (OpenSharedHandle underneath), and glReadPixels on it into a pixel buffer
 * object is the CopyTextureRegion + map a D3D12 presenter would do.
 *
 * One thread running a GLib main loop: the DRM node (readable when there is a
 * commit to fetch), the RDP server's source, a one millisecond tick while a
 * readback's fence is outstanding, and the session's clipboard on D-Bus
 * (selection.c).
 */

#define G_LOG_DOMAIN "weaselwayd"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#include "dxgdrm_drm.h"
#include "input.h"
#include "log.h"
#include "placeholder.h"
#include "rdp.h"
#include "selection.h"

#define MAX_IMPORTS 8

/* One imported scanout buffer. A compositor flips between a handful. */
struct import {
    uint64_t buffer_id;
    uint64_t last_used;
    EGLImage image;
    GLuint texture;
    GLuint fbo;
};

/* A readback that has been issued and whose pixels have not landed yet. At
 * most one at a time: damage that arrives meanwhile is kept and read with the
 * next one. */
struct readback {
    bool active;
    GLsync sync;
    struct rdp_rect rect;
    /* The buffer of the client's pool the pixels go to. */
    struct rdp_frame frame;
    uint64_t buffer_id;
    /* The screen's size when it was issued. */
    int width, height;
    gint64 started, issued;
};

struct weaselwayd {
    GMainLoop *loop;
    int exit_status;

    int drm_fd;
    struct gbm_device *gbm;
    EGLDisplay display;
    EGLContext context;
    /* GL_BGRA readback, which is what the client's buffers hold. Without it
     * the rows are swapped from RGBA while they are copied out. */
    bool read_bgra;

    struct import imports[MAX_IMPORTS];
    uint64_t use_counter;

    struct rdp_server *rdp;
    struct input *input;
    struct selection *selection;

    /* What is on the screen, as DXGDRM_GET_FRAME last described it. */
    uint64_t seq, primary_seq, cursor_seq;
    /* The frame the compositor has last been told we have taken. */
    uint64_t acked_seq;
    bool have_frame, have_cursor, cursor_sent, owned;
    uint64_t buffer_id;
    bool dumb;
    int width, height;
    uint32_t pitch;
    /* Where DXGDRM_READ_PIXELS puts a dumb frame; NULL until there is one. */
    uint8_t *dumb_data;
    size_t dumb_size;

    /* While no compositor owns the display the client gets placeholder.c's
     * screen, as large as the window it asked for. @placeholder_owed: it has
     * to be drawn (again). @placeholder_up: the client may be showing it, so
     * the compositor's screen has to be sent whole when one is back. */
    bool placeholder_owed, placeholder_up;
    int screen_width, screen_height;

    /* The part of the screen that changed and has not been read back. */
    bool pending;
    struct rdp_rect pending_rect;

    /* The pixel buffer glReadPixels targets, and the readback using it. */
    GLuint pbo;
    size_t pbo_size;
    struct readback readback;
    /* The source that looks at the readback's fence every millisecond; 0
     * while there is nothing to look at. */
    guint readback_tick;

    bool verbose;

    unsigned frames;
    /* Since the last statistics line. */
    unsigned stat_frames;
    double stat_ms, stat_pixels;
    gint64 stat_since;
    uint64_t stat_primary_seq;
};

/* An extension entry point, which libglvnd's libGLESv2 does not export. */
static void (*image_target_texture_2d)(GLenum target, void *image);

static double
ms_since(gint64 since, gint64 now)
{
    return (double)(now - since) / G_TIME_SPAN_MILLISECOND;
}

/* The dxgdrm render node. Matched on the driver name, as Mesa does. */
static int
open_dxgdrm(GError **error)
{
    for (int i = 128; i < 128 + 16; i++) {
        g_autofree char *path = g_strdup_printf("/dev/dri/renderD%d", i);
        g_autofd int fd = open(path, O_RDWR | O_CLOEXEC);
        drmVersionPtr version;
        bool match;

        if (fd < 0)
            continue;

        version = drmGetVersion(fd);
        match = version && !strcmp(version->name, "dxgdrm");
        drmFreeVersion(version);
        if (match) {
            g_message("using %s", path);
            return g_steal_fd(&fd);
        }
    }

    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "no dxgdrm render node -- is the module loaded?");
    return -1;
}

static bool
init_egl(struct weaselwayd *p, GError **error)
{
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_NONE,
    };
    const char *extensions;
    EGLint major, minor;

    p->gbm = gbm_create_device(p->drm_fd);
    if (!p->gbm) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "gbm_create_device failed");
        return false;
    }

    p->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, p->gbm, NULL);
    if (p->display == EGL_NO_DISPLAY || !eglInitialize(p->display, &major, &minor)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "no EGL display on the gbm device (0x%x)", eglGetError());
        return false;
    }

    extensions = eglQueryString(p->display, EGL_EXTENSIONS);
    if (!strstr(extensions, "EGL_EXT_image_dma_buf_import") ||
        !strstr(extensions, "EGL_KHR_surfaceless_context") ||
        !strstr(extensions, "EGL_KHR_no_config_context")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "EGL lacks dma-buf import, surfaceless or no-config contexts");
        return false;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    p->context = eglCreateContext(p->display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attribs);
    if (p->context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, p->context)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot create a GLES3 context (0x%x)",
                    eglGetError());
        return false;
    }

    image_target_texture_2d =
        (void (*)(GLenum, void *))eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!image_target_texture_2d || !strstr((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "GL lacks GL_OES_EGL_image");
        return false;
    }

    p->read_bgra = strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_read_format_bgra") != NULL;

    /* Every readback is tightly packed. These are GL's defaults, and nothing
     * here changes them. */
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);

    /* Only worth anything on the GPU: llvmpipe cannot open a D3D12 shared
     * handle. */
    g_message("EGL %d.%d, renderer: %s", major, minor, (const char *)glGetString(GL_RENDERER));
    return true;
}

static void
destroy_import(struct weaselwayd *p, struct import *import)
{
    if (!import->buffer_id)
        return;

    glDeleteFramebuffers(1, &import->fbo);
    glDeleteTextures(1, &import->texture);
    eglDestroyImage(p->display, import->image);
    memset(import, 0, sizeof(*import));
}

static struct import *
find_import(struct weaselwayd *p, uint64_t buffer_id)
{
    for (int i = 0; i < MAX_IMPORTS; i++) {
        if (p->imports[i].buffer_id == buffer_id)
            return &p->imports[i];
    }
    return NULL;
}

/* Import a shared handle, or find the import made for it earlier. Leaves its
 * framebuffer bound for reading. */
static struct import *
get_import(struct weaselwayd *p, uint64_t buffer_id, int fd, uint32_t width, uint32_t height,
           uint32_t format, uint32_t pitch)
{
    struct import *import = find_import(p, buffer_id);
    EGLAttrib attribs[] = {
        EGL_WIDTH, (EGLAttrib)width,
        EGL_HEIGHT, (EGLAttrib)height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLAttrib)format,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLAttrib)pitch,
        EGL_NONE,
    };

    if (!import) {
        /* In place of the one that has not been used for the longest. */
        import = &p->imports[0];
        for (int i = 1; i < MAX_IMPORTS; i++) {
            if (p->imports[i].last_used < import->last_used)
                import = &p->imports[i];
        }
        destroy_import(p, import);

        import->image = eglCreateImage(p->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                       NULL, attribs);
        if (import->image == EGL_NO_IMAGE) {
            g_warning("importing buffer %" G_GUINT64_FORMAT " failed (0x%x)", buffer_id,
                      eglGetError());
            return NULL;
        }

        glGenTextures(1, &import->texture);
        glBindTexture(GL_TEXTURE_2D, import->texture);
        image_target_texture_2d(GL_TEXTURE_2D, import->image);

        glGenFramebuffers(1, &import->fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, import->fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               import->texture, 0);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            g_warning("buffer %" G_GUINT64_FORMAT " is not readable as a framebuffer", buffer_id);
            import->buffer_id = buffer_id;
            destroy_import(p, import);
            return NULL;
        }

        import->buffer_id = buffer_id;
        g_message("imported buffer %" G_GUINT64_FORMAT " (%ux%u, %.4s)", buffer_id, width, height,
                  (const char *)&format);
    }

    import->last_used = ++p->use_counter;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, import->fbo);
    return import;
}

static void
add_damage(struct weaselwayd *p, int x1, int y1, int x2, int y2)
{
    struct rdp_rect rect;

    x1 = MAX(x1, 0);
    y1 = MAX(y1, 0);
    x2 = MIN(x2, p->width);
    y2 = MIN(y2, p->height);
    if (x2 <= x1 || y2 <= y1)
        return;

    rect = (struct rdp_rect){ x1, y1, x2 - x1, y2 - y1 };
    if (p->pending)
        rdp_rect_union(&p->pending_rect, &rect);
    else
        p->pending_rect = rect;
    p->pending = true;
}

/* @rows rows of @row_bytes, between two buffers whose rows are @dst_stride
 * and @src_stride bytes apart. */
static void
copy_rows(uint8_t *dst, size_t dst_stride, const uint8_t *src, size_t src_stride,
          size_t row_bytes, int rows)
{
    for (int y = 0; y < rows; y++)
        memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, row_bytes);
}

/* R G B A as B G R A, for a GL that cannot read the latter. In place too. */
static void
swap_red_blue(uint8_t *dst, const uint8_t *src, size_t pixels)
{
    for (size_t i = 0; i < pixels; i++, dst += 4, src += 4) {
        uint8_t r = src[0], b = src[2];

        dst[0] = b;
        dst[1] = src[1];
        dst[2] = r;
        dst[3] = src[3];
    }
}

static void
add_full_damage(struct weaselwayd *p)
{
    add_damage(p, 0, 0, p->width, p->height);
}

/* The frame's pixels are where they belong: show them, and say so. */
static void
frame_done(struct weaselwayd *p)
{
    struct readback *rb = &p->readback;
    gint64 done = g_get_monotonic_time();

    rdp_server_end_frame(p->rdp, &rb->frame, &rb->rect);
    p->frames++;

    p->stat_frames++;
    p->stat_ms += ms_since(rb->started, done);
    p->stat_pixels += (double)rb->rect.width * rb->rect.height;

    if (p->verbose)
        g_debug("frame %u: %s buffer %" G_GUINT64_FORMAT ", %dx%d+%d+%d (%.1f%%) read back in "
                "%.2f ms (issue %.2f)",
                p->frames, p->dumb ? "dumb" : "d3d12", rb->buffer_id, rb->rect.width,
                rb->rect.height, rb->rect.x, rb->rect.y,
                100.0 * rb->rect.width * rb->rect.height / ((double)p->width * p->height),
                ms_since(rb->started, done), ms_since(rb->started, rb->issued));
}

/* The frame did not make it; its part of the screen is still owed. */
static void
frame_failed(struct weaselwayd *p)
{
    struct readback *rb = &p->readback;

    rdp_server_cancel_frame(p->rdp, &rb->frame);
    add_damage(p, rb->rect.x, rb->rect.y, rb->rect.x + rb->rect.width,
               rb->rect.y + rb->rect.height);
}

/*
 * Issue the readback of @rect and return. Reading into a client pointer would
 * make the driver copy to a staging buffer, wait for the GPU, map and memcpy
 * before it returns. Reading into a pixel buffer object does not have to:
 * Mesa's d3d12 driver (weaselway's) issues a texture -> buffer copy and
 * returns. A fence says when the copy has landed, and the map after that does
 * not block.
 */
static gboolean on_readback_tick(gpointer data);

static bool
readback_begin(struct weaselwayd *p)
{
    struct readback *rb = &p->readback;
    struct import *import = find_import(p, p->buffer_id);
    size_t full_size = (size_t)p->width * (size_t)p->height * 4;

    if (!import)
        return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, import->fbo);

    if (!p->pbo)
        glGenBuffers(1, &p->pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo);
    /* Sized for a whole frame once, so that no damage shape reallocates it. */
    if (p->pbo_size != full_size) {
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)full_size, NULL, GL_STREAM_READ);
        p->pbo_size = full_size;
    }

    /* Tightly packed at offset 0: the transfer is exactly the rect. */
    glReadPixels(rb->rect.x, rb->rect.y, rb->rect.width, rb->rect.height,
                 p->read_bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    rb->sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    rb->issued = g_get_monotonic_time();
    rb->active = true;
    if (!p->readback_tick)
        p->readback_tick = g_timeout_add(1, on_readback_tick, p);
    return true;
}

/* Once the fence has signalled: copy the rect out of the pixel buffer. */
static bool
readback_copy(struct weaselwayd *p, uint8_t *pixels)
{
    const struct rdp_rect *rect = &p->readback.rect;
    size_t row_bytes = (size_t)rect->width * 4, stride = (size_t)p->width * 4;
    const uint8_t *src;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo);
    src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)(row_bytes * (size_t)rect->height),
                           GL_MAP_READ_BIT);
    if (!src) {
        g_warning("mapping the readback buffer failed");
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return false;
    }

    pixels += (size_t)rect->y * stride + (size_t)rect->x * 4;
    if (p->read_bgra) {
        copy_rows(pixels, stride, src, row_bytes, row_bytes, rect->height);
    } else {
        for (int y = 0; y < rect->height; y++)
            swap_red_blue(pixels + (size_t)y * stride, src + (size_t)y * row_bytes,
                          (size_t)rect->width);
    }

    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

/* Called every millisecond while a readback is outstanding. */
static void
readback_poll(struct weaselwayd *p)
{
    struct readback *rb = &p->readback;
    uint8_t *pixels;
    GLenum status;

    if (!rb->active)
        return;

    status = glClientWaitSync(rb->sync, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
    if (status == GL_TIMEOUT_EXPIRED && g_get_monotonic_time() - rb->issued < G_TIME_SPAN_SECOND)
        return;

    glDeleteSync(rb->sync);
    rb->active = false;

    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
        g_warning("the readback did not finish within a second");
        frame_failed(p);
        return;
    }

    /* The screen changed size under the readback; all of the new one is
     * owed already. */
    if (rb->width != p->width || rb->height != p->height) {
        rdp_server_cancel_frame(p->rdp, &rb->frame);
        return;
    }

    /* The client may have left, or been resized, while the pixels were on
     * their way. Whoever comes next asks for the whole screen anyway. */
    pixels = rdp_server_frame_pixels(p->rdp, &rb->frame);
    if (!pixels)
        return;

    if (readback_copy(p, pixels))
        frame_done(p);
    else
        frame_failed(p);
}

/* A dumb-buffer frame (a compositor rendering without the GPU). The kernel
 * only hands out all of it, into a buffer that is kept between frames; what
 * goes on to the client is @rect. XRGB8888 is B, G, R, X in memory, as
 * wanted. */
static bool
read_dumb(struct weaselwayd *p, uint8_t *pixels, const struct rdp_rect *rect)
{
    struct drm_dxgdrm_read_pixels read = { .plane = DXGDRM_PLANE_PRIMARY };
    size_t size = (size_t)p->pitch * (size_t)p->height;
    size_t stride = (size_t)p->width * 4;
    size_t offset = (size_t)rect->x * 4;

    if (p->dumb_size < size) {
        g_free(p->dumb_data);
        p->dumb_data = g_malloc(size);
        p->dumb_size = size;
    }
    read.size = (uint32_t)p->dumb_size;
    read.data = (uintptr_t)p->dumb_data;

    /* The plane may have moved on to another buffer since the frame was
     * fetched; the next fetch then says so. */
    if (drmIoctl(p->drm_fd, DRM_IOCTL_DXGDRM_READ_PIXELS, &read) ||
        (int)read.width != p->width || (int)read.height != p->height ||
        read.pitch < read.width * 4)
        return false;

    copy_rows(pixels + (size_t)rect->y * stride + offset, stride,
              p->dumb_data + (size_t)rect->y * read.pitch + offset, read.pitch,
              (size_t)rect->width * 4, rect->height);
    return true;
}

/*
 * The image on the cursor plane, tightly packed B G R A. mutter draws its
 * cursor into a dumb buffer, KWin renders it on the GPU like any other layer.
 * Small and only read when the image changes, so this one waits for the
 * pixels.
 */
static uint8_t *
read_cursor(struct weaselwayd *p, const struct drm_dxgdrm_get_frame *frame)
{
    int width = (int)frame->cursor_width, height = (int)frame->cursor_height;
    g_autofree uint8_t *pixels = g_malloc((size_t)width * (size_t)height * 4);

    if (frame->flags & DXGDRM_FRAME_CURSOR_SHARED) {
        if (!get_import(p, frame->cursor_buffer_id, frame->cursor_fd, frame->cursor_width,
                        frame->cursor_height, frame->cursor_format, frame->cursor_pitch))
            return NULL;

        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glReadPixels(0, 0, width, height, p->read_bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels);
        if (glGetError() != GL_NO_ERROR)
            return NULL;
        if (!p->read_bgra)
            swap_red_blue(pixels, pixels, (size_t)width * (size_t)height);
    } else {
        struct drm_dxgdrm_read_pixels read = { .plane = DXGDRM_PLANE_CURSOR };
        size_t size = (size_t)frame->cursor_pitch * (size_t)height;
        g_autofree uint8_t *data = g_malloc(size);

        read.size = (uint32_t)size;
        read.data = (uintptr_t)data;

        /* The plane may have moved on to another buffer since the frame was
         * fetched; the next fetch then says so. */
        if (drmIoctl(p->drm_fd, DRM_IOCTL_DXGDRM_READ_PIXELS, &read) ||
            (int)read.width != width || (int)read.height != height ||
            read.pitch < read.width * 4)
            return NULL;
        copy_rows(pixels, (size_t)width * 4, data, read.pitch, (size_t)width * 4, height);
    }
    return g_steal_pointer(&pixels);
}

/*
 * The cursor plane is the client's to draw: its image becomes the client's
 * mouse pointer, and no plane means no pointer. Only the image is sent. The
 * client moves the pointer itself, so a commit that only moves the plane is
 * nothing to it.
 *
 * A compositor makes the plane's buffer as large as the plane can be (256
 * square) and draws a much smaller cursor into its corner, so the image is cut
 * down to what is not transparent, with the hotspot kept inside.
 */
static void
update_cursor(struct weaselwayd *p, const struct drm_dxgdrm_get_frame *frame)
{
    int width = (int)frame->cursor_width, height = (int)frame->cursor_height;
    int hot_x = frame->cursor_hot_x, hot_y = frame->cursor_hot_y;
    int x1, y1, x2, y2;
    g_autofree uint8_t *pixels = NULL;

    if (!(frame->flags & DXGDRM_FRAME_CURSOR)) {
        if (p->have_cursor || !p->cursor_sent) {
            g_debug("cursor hidden");
            rdp_server_set_pointer(p->rdp, NULL, 0, 0, 0, 0, 0);
        }
        p->have_cursor = false;
        p->cursor_sent = true;
        return;
    }
    if (p->have_cursor && p->cursor_sent && frame->cursor_seq == p->cursor_seq)
        return;
    p->have_cursor = true;
    p->cursor_sent = true;
    p->cursor_seq = frame->cursor_seq;

    pixels = frame->cursor_format == DRM_FORMAT_ARGB8888 ? read_cursor(p, frame) : NULL;
    if (!pixels) {
        g_warning("cannot read the %dx%d cursor; hiding it", width, height);
        rdp_server_set_pointer(p->rdp, NULL, 0, 0, 0, 0, 0);
        return;
    }

    hot_x = CLAMP(hot_x, 0, width - 1);
    hot_y = CLAMP(hot_y, 0, height - 1);
    x1 = hot_x;
    y1 = hot_y;
    x2 = hot_x + 1;
    y2 = hot_y + 1;
    for (int y = 0; y < height; y++) {
        const uint8_t *row = pixels + (size_t)y * (size_t)width * 4;

        for (int x = 0; x < width; x++) {
            if (!row[x * 4 + 3])
                continue;
            x1 = MIN(x1, x);
            x2 = MAX(x2, x + 1);
            y1 = MIN(y1, y);
            y2 = MAX(y2, y + 1);
        }
    }

    g_debug("cursor updated: %dx%d %s buffer %" G_GUINT64_FORMAT ", hotspot %d,%d; sent as "
            "%dx%d+%d+%d",
            width, height, (frame->flags & DXGDRM_FRAME_CURSOR_SHARED) ? "d3d12" : "dumb",
            (guint64)frame->cursor_buffer_id, hot_x, hot_y, x2 - x1, y2 - y1, x1, y1);
    rdp_server_set_pointer(p->rdp, pixels + ((size_t)y1 * (size_t)width + (size_t)x1) * 4,
                           width * 4, x2 - x1, y2 - y1, hot_x - x1, hot_y - y1);
}

/*
 * Fetch what DXGDRM_GET_FRAME has to say. Only called when the node polls
 * readable, so it does not wait. False, with @error, if the node is gone.
 */
static bool
fetch_frame(struct weaselwayd *p, GError **error)
{
    struct drm_dxgdrm_get_frame frame = { .seq = p->seq, .timeout_ms = 1 };
    bool resized;

    if (ioctl(p->drm_fd, DRM_IOCTL_DXGDRM_GET_FRAME, &frame)) {
        if (errno == ETIME || errno == EINTR)
            return true;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "DXGDRM_GET_FRAME failed: %s", g_strerror(errno));
        return false;
    }
    p->seq = frame.seq;

    update_cursor(p, &frame);
    if (frame.cursor_fd >= 0)
        close(frame.cursor_fd);

    /* A compositor leaves its last frame up when it goes away. */
    if (!!(frame.flags & DXGDRM_FRAME_OWNED) != p->owned) {
        p->owned = !p->owned;
        g_message(p->owned ? "a compositor took over the display"
                           : "no compositor owns the display any more");
        if (!p->owned) {
            /* What it left on the screen is nobody's; the client is told so. */
            p->pending = false;
            p->placeholder_owed = true;
        }
    }

    if (!(frame.flags & DXGDRM_FRAME_PRIMARY)) {
        if (p->have_frame)
            g_message("the compositor turned the display off");
        p->have_frame = false;
        p->pending = false;
        return true;
    }

    resized = (int)frame.width != p->width || (int)frame.height != p->height;
    p->width = (int)frame.width;
    p->height = (int)frame.height;
    p->buffer_id = frame.buffer_id;
    p->dumb = !(frame.flags & DXGDRM_FRAME_SHARED);
    p->pitch = frame.pitch;
    if (!p->dumb) {
        g_clear_pointer(&p->dumb_data, g_free);
        p->dumb_size = 0;
    }

    /* The compositor has new buffers for the new size. Imports of the old
     * ones would keep those alive until they had all been pushed out. */
    if (resized) {
        for (int i = 0; i < MAX_IMPORTS; i++)
            destroy_import(p, &p->imports[i]);
    }

    /* Imported while the fd is at hand; the readback finds it by its id. */
    if (!p->dumb && !get_import(p, frame.buffer_id, frame.fd, frame.width, frame.height,
                                frame.format, frame.pitch)) {
        if (frame.fd >= 0)
            close(frame.fd);
        p->have_frame = false;
        return true;
    }
    if (frame.fd >= 0)
        close(frame.fd);

    if (!p->have_frame || resized) {
        /* Nothing that was read back before applies to this screen. */
        p->pending = false;
        add_full_damage(p);
    } else if (frame.primary_seq != p->primary_seq) {
        if (frame.flags & DXGDRM_FRAME_DAMAGE_FULL)
            add_full_damage(p);
        for (unsigned i = 0; i < frame.num_damage && i < DXGDRM_MAX_DAMAGE_RECTS; i++)
            add_damage(p, frame.damage[i].x1, frame.damage[i].y1, frame.damage[i].x2,
                       frame.damage[i].y2);
    }
    p->primary_seq = frame.primary_seq;
    p->have_frame = true;

    /* The client may still be showing the placeholder, which the damage of the
     * next commit does not cover. */
    if (p->owned && p->placeholder_up) {
        p->placeholder_up = p->placeholder_owed = false;
        add_full_damage(p);
    }
    return true;
}

/*
 * Tell the compositor that the frame on screen has been taken, which is what
 * lets its page flip complete and the next frame start (see
 * DXGDRM_ACK_FRAME). Withheld while damage is owed that cannot be read back
 * yet -- the previous readback has not landed, or the client holds every
 * buffer -- so a compositor faster than that waits. Not withheld for a client
 * that is still connecting: there is nothing to keep pace with.
 */
static void
ack_frame(struct weaselwayd *p)
{
    struct drm_dxgdrm_ack_frame ack = { .primary_seq = p->primary_seq };

    if (!p->have_frame || p->acked_seq == p->primary_seq)
        return;
    if (p->pending &&
        (p->readback.active || rdp_server_state(p->rdp) == RDP_BUSY))
        return;

    if (drmIoctl(p->drm_fd, DRM_IOCTL_DXGDRM_ACK_FRAME, &ack))
        g_warning("DXGDRM_ACK_FRAME failed: %s", g_strerror(errno));
    p->acked_seq = p->primary_seq;
}

/* The client's window has a size; ask for a screen of it. The compositor
 * switches to the new mode, and the frames that follow have that size. */
static void
apply_size_request(struct weaselwayd *p)
{
    struct drm_dxgdrm_set_mode mode;
    int width, height;

    if (!rdp_server_take_size_request(p->rdp, &width, &height))
        return;

    mode.width = (uint32_t)width;
    mode.height = (uint32_t)height;
    if (drmIoctl(p->drm_fd, DRM_IOCTL_DXGDRM_SET_MODE, &mode))
        g_warning("cannot set a %dx%d mode: %s", width, height, g_strerror(errno));
}

/* The size of the client's screen: the compositor's, or without one the
 * window the client asked for. */
static void
sync_screen_size(struct weaselwayd *p)
{
    int width = p->width, height = p->height;

    if (!p->owned)
        rdp_server_client_size(p->rdp, &width, &height);
    if (width == p->screen_width && height == p->screen_height)
        return;

    p->screen_width = width;
    p->screen_height = height;
    p->placeholder_owed = true;
    rdp_server_set_screen_size(p->rdp, width, height);
}

/* "No compositor running", in one frame and only when the client has not got
 * it yet: nothing changes on the screen until a compositor is back. */
static void
present_placeholder(struct weaselwayd *p)
{
    struct rdp_rect rect = { 0, 0, p->screen_width, p->screen_height };
    struct rdp_frame frame;
    uint8_t *pixels;

    if (rdp_server_take_full_request(p->rdp))
        p->placeholder_owed = true;
    if (!p->placeholder_owed || rdp_server_state(p->rdp) != RDP_READY)
        return;

    if (!rdp_server_begin_frame(p->rdp, &rect, &frame))
        return;
    pixels = rdp_server_frame_pixels(p->rdp, &frame);
    if (!pixels) {
        rdp_server_cancel_frame(p->rdp, &frame);
        return;
    }

    placeholder_draw(pixels, rect.width, rect.height);
    rdp_server_end_frame(p->rdp, &frame, &rect);
    p->placeholder_owed = false;
    p->placeholder_up = true;
    g_debug("no compositor: placeholder sent at %dx%d", rect.width, rect.height);
}

/*
 * Start on the damage that is owed, if there is somewhere to put it.
 *
 * One readback covers the bounding box of the damage, as mutter's RDP backend
 * does it; the wire carries a single rect anyway. The driver only takes the
 * direct path when a row is a multiple of 256 bytes (D3D12's placed-footprint
 * pitch), so the box is widened to a multiple of 64 pixels.
 */
static void
try_present(struct weaselwayd *p)
{
    struct readback *rb = &p->readback;
    enum rdp_state state = rdp_server_state(p->rdp);
    struct rdp_rect rect;
    uint8_t *pixels;

    if (rb->active)
        return;

    if (!p->owned) {
        present_placeholder(p);
        return;
    }

    if (!p->have_frame)
        return;

    if (rdp_server_take_full_request(p->rdp))
        add_full_damage(p);

    if (state == RDP_BUSY || state == RDP_CONNECTING || !p->pending)
        return;

    /* Nobody to show it to. A client that connects gets the whole screen. */
    if (state == RDP_NO_CLIENT) {
        p->pending = false;
        return;
    }

    rect = p->pending_rect;
    if (!p->dumb) {
        /* Widen to a multiple of 64 pixels, moving left where the right edge
         * is in the way. A screen narrower than that takes the slow path. */
        int w = MIN((rect.width + 63) & ~63, p->width);

        if (rect.x + w > p->width)
            rect.x = p->width - w;
        rect.width = w;
    }

    if (!rdp_server_begin_frame(p->rdp, &rect, &rb->frame))
        return;

    p->pending = false;
    rb->rect = rect;
    rb->buffer_id = p->buffer_id;
    rb->width = p->width;
    rb->height = p->height;
    rb->started = g_get_monotonic_time();

    if (!p->dumb) {
        if (!readback_begin(p))
            frame_failed(p);
        return;
    }

    rb->issued = rb->started;
    pixels = rdp_server_frame_pixels(p->rdp, &rb->frame);
    if (pixels && read_dumb(p, pixels, &rect))
        frame_done(p);
    else
        frame_failed(p);
}

/* What every source of the main loop ends with: something changed, so see
 * what can be done about the screen now. */
static void
pump(struct weaselwayd *p)
{
    apply_size_request(p);
    sync_screen_size(p);
    readback_poll(p);
    try_present(p);
    ack_frame(p);
}

static void
fail(struct weaselwayd *p, const GError *error)
{
    g_warning("%s", error->message);
    p->exit_status = EXIT_FAILURE;
    g_main_loop_quit(p->loop);
}

static gboolean
on_drm_ready(gint fd, GIOCondition condition, gpointer data)
{
    struct weaselwayd *p = data;
    g_autoptr(GError) error = NULL;

    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_CLOSED, "the dxgdrm node went away");
        fail(p, error);
        return G_SOURCE_REMOVE;
    }
    if (!fetch_frame(p, &error)) {
        fail(p, error);
        return G_SOURCE_REMOVE;
    }

    pump(p);
    return G_SOURCE_CONTINUE;
}

/* GLib's timeouts are no finer than a millisecond; the fence of a
 * damage-sized readback takes about two. */
static gboolean
on_readback_tick(gpointer data)
{
    struct weaselwayd *p = data;

    pump(p);
    if (p->readback.active)
        return G_SOURCE_CONTINUE;

    p->readback_tick = 0;
    return G_SOURCE_REMOVE;
}

static void
on_rdp_changed(void *data)
{
    pump(data);
}

static gboolean
on_stats(gpointer data)
{
    struct weaselwayd *p = data;
    gint64 now = g_get_monotonic_time();
    double screen = (double)p->width * p->height;

    /* With the compositor held to our pace the two counts match; more
     * commits than frames means frames were merged. */
    if (p->stat_frames && !p->verbose)
        g_message("%u frame(s) for %" G_GUINT64_FORMAT " commit(s) in %.0f s, %.2f ms and "
                  "%.0f%% of the screen each on average",
                  p->stat_frames, p->primary_seq - p->stat_primary_seq,
                  ms_since(p->stat_since, now) / 1000.0, p->stat_ms / p->stat_frames,
                  100.0 * p->stat_pixels / p->stat_frames / (screen > 0 ? screen : 1.0));
    p->stat_frames = 0;
    p->stat_primary_seq = p->primary_seq;
    p->stat_ms = p->stat_pixels = 0.0;
    p->stat_since = now;
    return G_SOURCE_CONTINUE;
}

static gboolean
on_signal(gpointer data)
{
    struct weaselwayd *p = data;

    g_main_loop_quit(p->loop);
    return G_SOURCE_CONTINUE;
}

int
main(int argc, char **argv)
{
    struct weaselwayd p = { .drm_fd = -1, .placeholder_owed = true };
    struct rdp_config rdp_config = {
        .vsock_port = 3389,
        .changed = on_rdp_changed,
        .changed_data = &p,
    };
    g_autofree char *shm_dir = NULL;
    gboolean no_input = FALSE, no_clipboard = FALSE, verbose = FALSE;
    const GOptionEntry entries[] = {
        { "port", 0, 0, G_OPTION_ARG_INT, &rdp_config.vsock_port,
          "The vsock port the RDP server listens on (default $WEASELWAY_VSOCK_PORT, or 3389)",
          "N" },
        { "tcp", 0, 0, G_OPTION_ARG_INT, &rdp_config.tcp_port,
          "Listen on 127.0.0.1:N instead of the vsock (debugging)", "N" },
        { "shm", 0, 0, G_OPTION_ARG_FILENAME, &shm_dir,
          "The shared-memory share the client maps the frames from (default "
          "$WSL2_SHARED_MEMORY_MOUNT_POINT, or /mnt/wslg-shared-memory)",
          "DIR" },
        { "no-input", 0, 0, G_OPTION_ARG_NONE, &no_input,
          "Create no uinput devices: no input from the client", NULL },
        { "no-clipboard", 0, 0, G_OPTION_ARG_NONE, &no_clipboard,
          "Leave the session's clipboard and the client's apart", NULL },
        { "verbose", 0, 0, G_OPTION_ARG_NONE, &verbose, "A line for every frame", NULL },
        G_OPTION_ENTRY_NULL,
    };
    g_autoptr(GOptionContext) options = g_option_context_new(NULL);
    g_autoptr(GError) error = NULL;
    const char *env;
    gint64 port;

    if ((env = g_getenv("WEASELWAY_VSOCK_PORT")) &&
        g_ascii_string_to_signed(env, 10, 1, G_MAXINT, &port, NULL))
        rdp_config.vsock_port = (int)port;

    g_option_context_set_summary(options, "The userspace half of dxgdrm's virtual display.");
    g_option_context_add_main_entries(options, entries, NULL);
    if (!g_option_context_parse(options, &argc, &argv, &error)) {
        g_printerr("%s\n", error->message);
        return 2;
    }
    if (argc > 1 || rdp_config.vsock_port <= 0 || rdp_config.tcp_port < 0 ||
        rdp_config.tcp_port > 65535) {
        g_autofree char *help = g_option_context_get_help(options, TRUE, NULL);

        g_printerr("%s", help);
        return 2;
    }

    if (!shm_dir) {
        env = g_getenv("WSL2_SHARED_MEMORY_MOUNT_POINT");
        shm_dir = g_strdup(env && *env ? env : "/mnt/wslg-shared-memory");
    }
    rdp_config.shm_dir = shm_dir;
    rdp_config.verbose = p.verbose = verbose;
    log_init(verbose);

    /* A client that goes away mid-write must not take weaselwayd with it. */
    signal(SIGPIPE, SIG_IGN);

    p.drm_fd = open_dxgdrm(&error);
    if (p.drm_fd < 0 || !init_egl(&p, &error)) {
        g_warning("%s", error->message);
        return EXIT_FAILURE;
    }

    if (!no_input)
        p.input = input_new();
    if (!no_clipboard)
        p.selection = selection_new();

    rdp_config.input = p.input;
    rdp_config.selection = p.selection;
    p.rdp = rdp_server_new(&rdp_config, &error);
    if (!p.rdp) {
        g_warning("%s", error->message);
        return EXIT_FAILURE;
    }

    /* The first call is what makes the node poll readable from then on. It
     * returns at once if a compositor is already up. */
    if (!fetch_frame(&p, &error)) {
        g_warning("%s", error->message);
        return EXIT_FAILURE;
    }
    if (!p.have_frame)
        g_message("waiting for the compositor's first commit");
    p.stat_since = g_get_monotonic_time();
    p.stat_primary_seq = p.primary_seq;

    p.loop = g_main_loop_new(NULL, FALSE);
    g_unix_fd_add(p.drm_fd, G_IO_IN, on_drm_ready, &p);
    g_unix_signal_add(SIGINT, on_signal, &p);
    g_unix_signal_add(SIGTERM, on_signal, &p);
    g_timeout_add_seconds(5, on_stats, &p);
    pump(&p);

    g_main_loop_run(p.loop);

    g_clear_handle_id(&p.readback_tick, g_source_remove);
    if (p.readback.active)
        glDeleteSync(p.readback.sync);
    rdp_server_free(p.rdp);
    selection_free(p.selection);
    input_free(p.input);
    for (int i = 0; i < MAX_IMPORTS; i++)
        destroy_import(&p, &p.imports[i]);
    g_free(p.dumb_data);
    g_main_loop_unref(p.loop);
    g_message("%u frame(s)", p.frames);
    return p.exit_status;
}
