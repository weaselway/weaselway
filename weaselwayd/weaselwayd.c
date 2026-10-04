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
 * One thread, one poll(): the DRM node (readable when there is a commit to
 * fetch), FreeRDP's descriptors, and a one millisecond tick while a readback's
 * fence is outstanding.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>


#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#include "dxgdrm_drm.h"
#include "input.h"
#include "rdp.h"

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
    double started, issued;
};

struct weaselwayd {
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

    /* What is on the screen, as DXGDRM_GET_FRAME last described it. */
    uint64_t seq, primary_seq, cursor_seq;
    /* The frame the compositor has last been told we have taken. */
    uint64_t acked_seq;
    bool have_frame, have_cursor, cursor_sent, owned;
    uint64_t buffer_id;
    bool dumb;
    int width, height;

    /* The part of the screen that changed and has not been read back. */
    bool pending;
    struct rdp_rect pending_rect;

    /* The pixel buffer glReadPixels targets, and the readback using it. */
    GLuint pbo;
    size_t pbo_size;
    struct readback readback;

    bool verbose;

    unsigned frames;
    /* Since the last statistics line. */
    unsigned stat_frames;
    double stat_ms, stat_pixels, stat_since;
    uint64_t stat_primary_seq;
};

static volatile sig_atomic_t quit;

/* An extension entry point, which libglvnd's libGLESv2 does not export. */
static void (*image_target_texture_2d)(GLenum target, void *image);

static void
on_signal(int sig)
{
    quit = 1;
}

static double
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* The dxgdrm render node. Matched on the driver name, as Mesa does. */
static int
open_dxgdrm(void)
{
    for (int i = 128; i < 128 + 16; i++) {
        char path[64];
        drmVersionPtr version;
        int fd;
        bool match;

        snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        version = drmGetVersion(fd);
        match = version && !strcmp(version->name, "dxgdrm");
        drmFreeVersion(version);
        if (match) {
            fprintf(stderr, "weaselwayd: using %s\n", path);
            return fd;
        }
        close(fd);
    }

    fprintf(stderr, "weaselwayd: no dxgdrm render node -- is the module loaded?\n");
    return -1;
}

static bool
init_egl(struct weaselwayd *p)
{
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_NONE,
    };
    const char *extensions;
    EGLint major, minor;

    p->gbm = gbm_create_device(p->drm_fd);
    if (!p->gbm) {
        fprintf(stderr, "weaselwayd: gbm_create_device failed\n");
        return false;
    }

    p->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, p->gbm, NULL);
    if (p->display == EGL_NO_DISPLAY || !eglInitialize(p->display, &major, &minor)) {
        fprintf(stderr, "weaselwayd: no EGL display on the gbm device (0x%x)\n", eglGetError());
        return false;
    }

    extensions = eglQueryString(p->display, EGL_EXTENSIONS);
    if (!strstr(extensions, "EGL_EXT_image_dma_buf_import") ||
        !strstr(extensions, "EGL_KHR_surfaceless_context") ||
        !strstr(extensions, "EGL_KHR_no_config_context")) {
        fprintf(stderr, "weaselwayd: EGL lacks dma-buf import, surfaceless or no-config contexts\n");
        return false;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    p->context = eglCreateContext(p->display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attribs);
    if (p->context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, p->context)) {
        fprintf(stderr, "weaselwayd: cannot create a GLES3 context (0x%x)\n", eglGetError());
        return false;
    }

    image_target_texture_2d =
        (void (*)(GLenum, void *))eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!image_target_texture_2d || !strstr((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image")) {
        fprintf(stderr, "weaselwayd: GL lacks GL_OES_EGL_image\n");
        return false;
    }

    p->read_bgra = strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_read_format_bgra") != NULL;

    /* Only worth anything on the GPU: llvmpipe cannot open a D3D12 shared
     * handle. */
    fprintf(stderr, "weaselwayd: EGL %d.%d, renderer: %s\n", major, minor,
            (const char *)glGetString(GL_RENDERER));
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

/* Import the frame's shared handle, or find the import made for it earlier.
 * Leaves its framebuffer bound for reading. */
static struct import *
get_import_buffer(struct weaselwayd *p, uint64_t buffer_id, int fd,
                  uint32_t width, uint32_t height, uint32_t format, uint32_t pitch)
{
    struct import *import = NULL, *oldest = &p->imports[0];
    EGLAttrib attribs[] = {
        EGL_WIDTH, (EGLAttrib)width,
        EGL_HEIGHT, (EGLAttrib)height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLAttrib)format,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLAttrib)pitch,
        EGL_NONE,
    };

    for (int i = 0; i < MAX_IMPORTS; i++) {
        if (p->imports[i].buffer_id == buffer_id) {
            import = &p->imports[i];
            break;
        }
        if (p->imports[i].last_used < oldest->last_used)
            oldest = &p->imports[i];
    }

    if (!import) {
        import = oldest;
        destroy_import(p, import);

        import->image = eglCreateImage(p->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                       NULL, attribs);
        if (import->image == EGL_NO_IMAGE) {
            fprintf(stderr, "weaselwayd: importing buffer %llu failed (0x%x)\n",
                    (unsigned long long)buffer_id, eglGetError());
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
            fprintf(stderr, "weaselwayd: buffer %llu is not readable as a framebuffer\n",
                    (unsigned long long)buffer_id);
            import->buffer_id = buffer_id;
            destroy_import(p, import);
            return NULL;
        }

        import->buffer_id = buffer_id;
        fprintf(stderr, "weaselwayd: imported buffer %llu (%ux%u, %.4s)\n",
                (unsigned long long)buffer_id, width, height,
                (const char *)&format);
    }

    import->last_used = ++p->use_counter;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, import->fbo);
    return import;
}

static struct import *
get_import(struct weaselwayd *p, const struct drm_dxgdrm_get_frame *frame)
{
    return get_import_buffer(p, frame->buffer_id, frame->fd, frame->width, frame->height,
                             frame->format, frame->pitch);
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

static void
add_damage(struct weaselwayd *p, int x1, int y1, int x2, int y2)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > p->width) x2 = p->width;
    if (y2 > p->height) y2 = p->height;
    if (x2 <= x1 || y2 <= y1)
        return;

    if (p->pending) {
        struct rdp_rect *r = &p->pending_rect;
        int px2 = r->x + r->width, py2 = r->y + r->height;

        if (r->x < x1) x1 = r->x;
        if (r->y < y1) y1 = r->y;
        if (px2 > x2) x2 = px2;
        if (py2 > y2) y2 = py2;
    }
    p->pending_rect = (struct rdp_rect){ x1, y1, x2 - x1, y2 - y1 };
    p->pending = true;
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
    double done = now_ms();

    rdp_server_end_frame(p->rdp, &rb->frame, &rb->rect);
    p->frames++;

    p->stat_frames++;
    p->stat_ms += done - rb->started;
    p->stat_pixels += (double)rb->rect.width * rb->rect.height;

    if (p->verbose)
        fprintf(stderr,
                "frame %u: %s buffer %llu, %dx%d+%d+%d (%.1f%%) read back in %.2f ms "
                "(issue %.2f)\n",
                p->frames, p->dumb ? "dumb" : "d3d12", (unsigned long long)rb->buffer_id,
                rb->rect.width, rb->rect.height, rb->rect.x, rb->rect.y,
                100.0 * rb->rect.width * rb->rect.height / ((double)p->width * p->height),
                done - rb->started, rb->issued - rb->started);
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
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(rb->rect.x, rb->rect.y, rb->rect.width, rb->rect.height,
                 p->read_bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    rb->sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    rb->issued = now_ms();
    rb->active = true;
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
        fprintf(stderr, "weaselwayd: mapping the readback buffer failed\n");
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return false;
    }

    for (int y = 0; y < rect->height; y++) {
        uint8_t *dst = pixels + (size_t)(rect->y + y) * stride + (size_t)rect->x * 4;
        const uint8_t *row = src + (size_t)y * row_bytes;

        if (p->read_bgra) {
            memcpy(dst, row, row_bytes);
        } else {
            for (int x = 0; x < rect->width; x++, dst += 4, row += 4) {
                dst[0] = row[2];
                dst[1] = row[1];
                dst[2] = row[0];
                dst[3] = row[3];
            }
        }
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
    if (status == GL_TIMEOUT_EXPIRED && now_ms() - rb->issued < 1000.0)
        return;

    glDeleteSync(rb->sync);
    rb->active = false;

    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
        fprintf(stderr, "weaselwayd: the readback did not finish within a second\n");
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

/* A dumb-buffer frame (a compositor rendering without the GPU): copy all of
 * it out of the kernel. XRGB8888 is B, G, R, X in memory, as wanted. */
static bool
read_dumb(struct weaselwayd *p, uint8_t *pixels)
{
    struct drm_dxgdrm_read_pixels read = { .plane = DXGDRM_PLANE_PRIMARY };
    size_t size = (size_t)p->width * (size_t)p->height * 4 * 2;
    uint8_t *data = malloc(size);
    bool ok = false;

    if (!data)
        return false;
    read.size = (uint32_t)size;
    read.data = (uintptr_t)data;

    if (!drmIoctl(p->drm_fd, DRM_IOCTL_DXGDRM_READ_PIXELS, &read) &&
        (int)read.width == p->width && (int)read.height == p->height &&
        read.pitch >= read.width * 4) {
        for (int y = 0; y < p->height; y++)
            memcpy(pixels + (size_t)y * (size_t)p->width * 4, data + (size_t)y * read.pitch,
                   (size_t)p->width * 4);
        ok = true;
    }

    free(data);
    return ok;
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
    uint8_t *pixels = malloc((size_t)width * (size_t)height * 4);

    if (!pixels)
        return NULL;

    if (frame->flags & DXGDRM_FRAME_CURSOR_SHARED) {
        if (!get_import_buffer(p, frame->cursor_buffer_id, frame->cursor_fd, frame->cursor_width,
                               frame->cursor_height, frame->cursor_format, frame->cursor_pitch))
            goto fail;

        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glReadPixels(0, 0, width, height, p->read_bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels);
        if (glGetError() != GL_NO_ERROR)
            goto fail;
        if (!p->read_bgra) {
            for (uint8_t *px = pixels, *end = pixels + (size_t)width * (size_t)height * 4;
                 px < end; px += 4) {
                uint8_t r = px[0];

                px[0] = px[2];
                px[2] = r;
            }
        }
    } else {
        struct drm_dxgdrm_read_pixels read = { .plane = DXGDRM_PLANE_CURSOR };
        size_t size = (size_t)frame->cursor_pitch * (size_t)height;
        uint8_t *data = malloc(size);

        if (!data)
            goto fail;
        read.size = (uint32_t)size;
        read.data = (uintptr_t)data;

        /* The plane may have moved on to another buffer since the frame was
         * fetched; the next fetch then says so. */
        if (drmIoctl(p->drm_fd, DRM_IOCTL_DXGDRM_READ_PIXELS, &read) ||
            (int)read.width != width || (int)read.height != height ||
            read.pitch < read.width * 4) {
            free(data);
            goto fail;
        }
        for (int y = 0; y < height; y++)
            memcpy(pixels + (size_t)y * (size_t)width * 4, data + (size_t)y * read.pitch,
                   (size_t)width * 4);
        free(data);
    }
    return pixels;

fail:
    free(pixels);
    return NULL;
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
    uint8_t *pixels;

    if (!(frame->flags & DXGDRM_FRAME_CURSOR)) {
        if (p->have_cursor || !p->cursor_sent) {
            if (p->verbose)
                fprintf(stderr, "weaselwayd: cursor hidden\n");
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
        fprintf(stderr, "weaselwayd: cannot read the %dx%d cursor; hiding it\n", width, height);
        rdp_server_set_pointer(p->rdp, NULL, 0, 0, 0, 0, 0);
        return;
    }

    hot_x = hot_x < 0 ? 0 : hot_x >= width ? width - 1 : hot_x;
    hot_y = hot_y < 0 ? 0 : hot_y >= height ? height - 1 : hot_y;
    x1 = hot_x;
    y1 = hot_y;
    x2 = hot_x + 1;
    y2 = hot_y + 1;
    for (int y = 0; y < height; y++) {
        const uint8_t *row = pixels + (size_t)y * (size_t)width * 4;

        for (int x = 0; x < width; x++) {
            if (!row[x * 4 + 3])
                continue;
            if (x < x1)
                x1 = x;
            if (x >= x2)
                x2 = x + 1;
            if (y < y1)
                y1 = y;
            if (y >= y2)
                y2 = y + 1;
        }
    }

    if (p->verbose)
        fprintf(stderr, "weaselwayd: cursor updated: %dx%d %s buffer %llu, hotspot %d,%d; "
                        "sent as %dx%d+%d+%d\n",
                width, height, (frame->flags & DXGDRM_FRAME_CURSOR_SHARED) ? "d3d12" : "dumb",
                (unsigned long long)frame->cursor_buffer_id, hot_x, hot_y, x2 - x1, y2 - y1, x1,
                y1);
    rdp_server_set_pointer(p->rdp, pixels + ((size_t)y1 * (size_t)width + (size_t)x1) * 4,
                           width * 4, x2 - x1, y2 - y1, hot_x - x1, hot_y - y1);
    free(pixels);
}

/*
 * Fetch what DXGDRM_GET_FRAME has to say. Only called when the node polls
 * readable, so it does not wait. Returns false if the node is gone.
 */
static bool
fetch_frame(struct weaselwayd *p)
{
    struct drm_dxgdrm_get_frame frame = { .seq = p->seq, .timeout_ms = 1 };
    bool resized;

    if (ioctl(p->drm_fd, DRM_IOCTL_DXGDRM_GET_FRAME, &frame)) {
        if (errno == ETIME || errno == EINTR)
            return true;
        fprintf(stderr, "weaselwayd: DXGDRM_GET_FRAME failed: %s\n", strerror(errno));
        return false;
    }
    p->seq = frame.seq;

    update_cursor(p, &frame);
    if (frame.cursor_fd >= 0)
        close(frame.cursor_fd);

    /* A compositor leaves its last frame up when it goes away. */
    if (!!(frame.flags & DXGDRM_FRAME_OWNED) != p->owned) {
        p->owned = !p->owned;
        fprintf(stderr, p->owned ? "weaselwayd: a compositor took over the display\n"
                                 : "weaselwayd: no compositor owns the display any more\n");
    }

    if (!(frame.flags & DXGDRM_FRAME_PRIMARY)) {
        if (p->have_frame)
            fprintf(stderr, "weaselwayd: the compositor turned the display off\n");
        p->have_frame = false;
        p->pending = false;
        return true;
    }

    resized = (int)frame.width != p->width || (int)frame.height != p->height;
    p->width = (int)frame.width;
    p->height = (int)frame.height;
    p->buffer_id = frame.buffer_id;
    p->dumb = !(frame.flags & DXGDRM_FRAME_SHARED);
    rdp_server_set_screen_size(p->rdp, p->width, p->height);

    /* Imported while the fd is at hand; the readback finds it by its id. */
    if (!p->dumb && !get_import(p, &frame)) {
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
        fprintf(stderr, "weaselwayd: DXGDRM_ACK_FRAME failed: %s\n", strerror(errno));
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
        fprintf(stderr, "weaselwayd: cannot set a %dx%d mode: %s\n", width, height, strerror(errno));
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

    if (!p->have_frame || rb->active)
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
    if (p->dumb) {
        rect = (struct rdp_rect){ 0, 0, p->width, p->height };
    } else {
        /* Widen to a multiple of 64 pixels, moving left where the right edge
         * is in the way. A screen narrower than that takes the slow path. */
        int w = (rect.width + 63) & ~63;

        if (w > p->width)
            w = p->width;
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
    rb->started = now_ms();

    if (!p->dumb) {
        if (!readback_begin(p))
            frame_failed(p);
        return;
    }

    rb->issued = rb->started;
    pixels = rdp_server_frame_pixels(p->rdp, &rb->frame);
    if (pixels && read_dumb(p, pixels))
        frame_done(p);
    else
        frame_failed(p);
}

static void
usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --port N        vsock port the RDP server listens on (default\n"
            "                  $MUTTER_RDP_VSOCK_PORT, or 3389)\n"
            "  --tcp N         listen on 127.0.0.1:N instead of the vsock (debugging)\n"
            "  --shm DIR       the shared-memory share the client maps the frames from\n"
            "                  (default $WSL2_SHARED_MEMORY_MOUNT_POINT, or\n"
            "                  /mnt/wslg-shared-memory)\n"
            "  --no-input      create no uinput devices: no input from the client\n"
            "  --verbose       a line for every frame\n",
            argv0);
}

int
main(int argc, char **argv)
{
    struct weaselwayd p = { 0 };
    struct rdp_config rdp_config = {
        .vsock_port = 3389,
        .shm_dir = "/mnt/wslg-shared-memory",
    };
    struct sigaction action = { .sa_handler = on_signal };
    bool use_input = true;
    const char *env;

    if ((env = getenv("MUTTER_RDP_VSOCK_PORT")) && atoi(env) > 0)
        rdp_config.vsock_port = atoi(env);
    if ((env = getenv("WSL2_SHARED_MEMORY_MOUNT_POINT")) && *env)
        rdp_config.shm_dir = env;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            rdp_config.vsock_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--tcp") && i + 1 < argc) {
            rdp_config.tcp_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--shm") && i + 1 < argc) {
            rdp_config.shm_dir = argv[++i];
        } else if (!strcmp(argv[i], "--no-input")) {
            use_input = false;
        } else if (!strcmp(argv[i], "--verbose")) {
            p.verbose = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (rdp_config.vsock_port <= 0 || rdp_config.tcp_port < 0 || rdp_config.tcp_port > 65535) {
        usage(argv[0]);
        return 2;
    }

    /* No SA_RESTART: a signal has to get us out of poll(). A client that goes
     * away mid-write must not take weaselwayd with it. */
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    p.drm_fd = open_dxgdrm();
    if (p.drm_fd < 0 || !init_egl(&p))
        return 1;

    if (use_input)
        p.input = input_new();

    rdp_config.input = p.input;
    rdp_config.verbose = p.verbose;
    p.rdp = rdp_server_new(&rdp_config);
    if (!p.rdp)
        return 1;

    /* The first call is what makes the node poll readable from then on. It
     * returns at once if a compositor is already up. */
    if (!fetch_frame(&p))
        return 1;
    if (!p.have_frame)
        fprintf(stderr, "weaselwayd: waiting for the compositor's first commit\n");
    p.stat_since = now_ms();
    p.stat_primary_seq = p.primary_seq;

    while (!quit) {
        struct pollfd fds[1 + 80];
        int n = 1, rdp_n = 0, timeout = 1000, rdp_timeout;

        fds[0] = (struct pollfd){ .fd = p.drm_fd, .events = POLLIN };
        rdp_n = rdp_server_get_fds(p.rdp, &fds[1], 80);
        n += rdp_n;
        rdp_timeout = rdp_server_timeout_ms(p.rdp);
        if (rdp_timeout >= 0 && rdp_timeout < timeout)
            timeout = rdp_timeout;
        /* GLib rounds its timeouts to a millisecond too; the fence of a
         * damage-sized readback takes about two. */
        if (p.readback.active)
            timeout = 1;

        if (poll(fds, (nfds_t)n, timeout) < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "weaselwayd: poll failed: %s\n", strerror(errno));
            break;
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "weaselwayd: the dxgdrm node went away\n");
            break;
        }
        if ((fds[0].revents & POLLIN) && !fetch_frame(&p))
            break;

        rdp_server_dispatch(p.rdp, &fds[1], rdp_n);
        apply_size_request(&p);

        readback_poll(&p);
        try_present(&p);
        ack_frame(&p);

        if (now_ms() - p.stat_since >= 5000.0) {
            /* With the compositor held to our pace the two counts match;
             * more commits than frames means frames were merged. */
            if (p.stat_frames && !p.verbose)
                fprintf(stderr, "weaselwayd: %u frame(s) for %llu commit(s) in %.0f s, %.2f ms "
                                "and %.0f%% of the screen each on average\n",
                        p.stat_frames,
                        (unsigned long long)(p.primary_seq - p.stat_primary_seq),
                        (now_ms() - p.stat_since) / 1000.0,
                        p.stat_ms / p.stat_frames,
                        100.0 * p.stat_pixels / p.stat_frames /
                            ((double)p.width * p.height > 0 ? (double)p.width * p.height : 1.0));
            p.stat_frames = 0;
            p.stat_primary_seq = p.primary_seq;
            p.stat_ms = p.stat_pixels = 0.0;
            p.stat_since = now_ms();
        }
    }

    if (p.readback.active)
        glDeleteSync(p.readback.sync);
    rdp_server_free(p.rdp);
    input_free(p.input);
    for (int i = 0; i < MAX_IMPORTS; i++)
        destroy_import(&p, &p.imports[i]);
    fprintf(stderr, "weaselwayd: %u frame(s)\n", p.frames);
    return 0;
}
