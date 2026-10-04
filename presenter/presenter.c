/*
 * weaselway-presenter: the userspace half of dxgdrm's virtual display.
 *
 * The compositor scans out to dxgdrm's KMS node. This waits for its commits,
 * reads the damaged part of each frame back and hands it to the Windows client
 * through gfxredir shared memory (rdp.c); the client's mouse and keyboard come
 * back as uinput devices (input.c). With --out it also, or instead, writes
 * every frame to a JPEG.
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
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <jpeglib.h>
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
    /* Where the pixels go: a buffer of the client's pool, or the shadow copy. */
    bool to_client;
    struct rdp_frame frame;
    uint64_t buffer_id;
    /* The screen's size when it was issued. */
    int width, height;
    double started, issued;
};

struct presenter {
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
    bool have_frame, have_cursor, owned;
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

    /* For --out without a client: the screen as last read back, B G R X. */
    uint8_t *shadow;
    int shadow_width, shadow_height;

    const char *out_dir;
    unsigned max_frames;
    int quality;
    bool verbose;

    unsigned frames;
    /* Since the last statistics line. */
    unsigned stat_frames;
    double stat_ms, stat_pixels, stat_since;
};

static volatile sig_atomic_t quit;

/* When to click once in the middle of the screen, as now_ms(); 0 for never.
 * Set by the main loop, taken by the pointer thread. */
static atomic_long click_at_ms;

/* GNOME Shell starts in the overview, which is not much to look at. A click on
 * the workspace in the middle of it leaves the overview, and is harmless
 * anywhere else. */
#define CLICK_DELAY_MS 3000

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
            fprintf(stderr, "presenter: using %s\n", path);
            return fd;
        }
        close(fd);
    }

    fprintf(stderr, "presenter: no dxgdrm render node -- is the module loaded?\n");
    return -1;
}

static bool
init_egl(struct presenter *p)
{
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_NONE,
    };
    const char *extensions;
    EGLint major, minor;

    p->gbm = gbm_create_device(p->drm_fd);
    if (!p->gbm) {
        fprintf(stderr, "presenter: gbm_create_device failed\n");
        return false;
    }

    p->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, p->gbm, NULL);
    if (p->display == EGL_NO_DISPLAY || !eglInitialize(p->display, &major, &minor)) {
        fprintf(stderr, "presenter: no EGL display on the gbm device (0x%x)\n", eglGetError());
        return false;
    }

    extensions = eglQueryString(p->display, EGL_EXTENSIONS);
    if (!strstr(extensions, "EGL_EXT_image_dma_buf_import") ||
        !strstr(extensions, "EGL_KHR_surfaceless_context") ||
        !strstr(extensions, "EGL_KHR_no_config_context")) {
        fprintf(stderr, "presenter: EGL lacks dma-buf import, surfaceless or no-config contexts\n");
        return false;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    p->context = eglCreateContext(p->display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attribs);
    if (p->context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, p->context)) {
        fprintf(stderr, "presenter: cannot create a GLES3 context (0x%x)\n", eglGetError());
        return false;
    }

    image_target_texture_2d =
        (void (*)(GLenum, void *))eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!image_target_texture_2d || !strstr((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image")) {
        fprintf(stderr, "presenter: GL lacks GL_OES_EGL_image\n");
        return false;
    }

    p->read_bgra = strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_read_format_bgra") != NULL;

    /* Only worth anything on the GPU: llvmpipe cannot open a D3D12 shared
     * handle. */
    fprintf(stderr, "presenter: EGL %d.%d, renderer: %s\n", major, minor,
            (const char *)glGetString(GL_RENDERER));
    return true;
}

static void
destroy_import(struct presenter *p, struct import *import)
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
get_import_buffer(struct presenter *p, uint64_t buffer_id, int fd,
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
            fprintf(stderr, "presenter: importing buffer %llu failed (0x%x)\n",
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
            fprintf(stderr, "presenter: buffer %llu is not readable as a framebuffer\n",
                    (unsigned long long)buffer_id);
            import->buffer_id = buffer_id;
            destroy_import(p, import);
            return NULL;
        }

        import->buffer_id = buffer_id;
        fprintf(stderr, "presenter: imported buffer %llu (%ux%u, %.4s)\n",
                (unsigned long long)buffer_id, width, height,
                (const char *)&format);
    }

    import->last_used = ++p->use_counter;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, import->fbo);
    return import;
}

static struct import *
get_import(struct presenter *p, const struct drm_dxgdrm_get_frame *frame)
{
    return get_import_buffer(p, frame->buffer_id, frame->fd, frame->width, frame->height,
                             frame->format, frame->pitch);
}

static struct import *
find_import(struct presenter *p, uint64_t buffer_id)
{
    for (int i = 0; i < MAX_IMPORTS; i++) {
        if (p->imports[i].buffer_id == buffer_id)
            return &p->imports[i];
    }
    return NULL;
}

static bool
resize_shadow(struct presenter *p)
{
    if (p->shadow && p->shadow_width == p->width && p->shadow_height == p->height)
        return true;

    free(p->shadow);
    p->shadow = calloc((size_t)p->width * (size_t)p->height, 4);
    p->shadow_width = p->width;
    p->shadow_height = p->height;
    return p->shadow != NULL;
}

static void
add_damage(struct presenter *p, int x1, int y1, int x2, int y2)
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
add_full_damage(struct presenter *p)
{
    add_damage(p, 0, 0, p->width, p->height);
}

static bool
write_jpeg(struct presenter *p, const uint8_t *pixels, unsigned index)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    char path[4096], tmp[4096 + 8];
    FILE *file;

    snprintf(path, sizeof(path), "%s/frame-%06u.jpg", p->out_dir, index);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    file = fopen(tmp, "wb");
    if (!file) {
        fprintf(stderr, "presenter: cannot write %s: %s\n", tmp, strerror(errno));
        return false;
    }

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, file);
    cinfo.image_width = (JDIMENSION)p->width;
    cinfo.image_height = (JDIMENSION)p->height;
    cinfo.input_components = 4;
    cinfo.in_color_space = JCS_EXT_BGRX;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, p->quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)(pixels + (size_t)cinfo.next_scanline * (size_t)p->width * 4);

        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(file);

    /* So a viewer following the directory never sees half a file. */
    return rename(tmp, path) == 0;
}

/* The frame's pixels are where they belong: show them, and say so. */
static void
frame_done(struct presenter *p, const uint8_t *pixels)
{
    struct readback *rb = &p->readback;
    double done = now_ms(), jpeg_ms = 0.0;

    if (rb->to_client)
        rdp_server_end_frame(p->rdp, &rb->frame, &rb->rect);

    if (p->out_dir) {
        double t = now_ms();

        write_jpeg(p, pixels, p->frames % p->max_frames);
        jpeg_ms = now_ms() - t;
    }
    p->frames++;

    p->stat_frames++;
    p->stat_ms += done - rb->started;
    p->stat_pixels += (double)rb->rect.width * rb->rect.height;

    if (p->verbose)
        fprintf(stderr,
                "frame %u: %s buffer %llu, %dx%d+%d+%d (%.1f%%) read back in %.2f ms "
                "(issue %.2f), jpeg %.2f ms\n",
                p->frames, p->dumb ? "dumb" : "d3d12", (unsigned long long)rb->buffer_id,
                rb->rect.width, rb->rect.height, rb->rect.x, rb->rect.y,
                100.0 * rb->rect.width * rb->rect.height / ((double)p->width * p->height),
                done - rb->started, rb->issued - rb->started, jpeg_ms);
}

/* The frame did not make it; its part of the screen is still owed. */
static void
frame_failed(struct presenter *p)
{
    struct readback *rb = &p->readback;

    if (rb->to_client)
        rdp_server_cancel_frame(p->rdp, &rb->frame);
    add_damage(p, rb->rect.x, rb->rect.y, rb->rect.x + rb->rect.width,
               rb->rect.y + rb->rect.height);
}

/* Where the frame in progress goes, or NULL if that place is gone. */
static uint8_t *
frame_pixels(struct presenter *p)
{
    struct readback *rb = &p->readback;

    if (rb->to_client)
        return rdp_server_frame_pixels(p->rdp, &rb->frame);
    return p->shadow;
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
readback_begin(struct presenter *p)
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
readback_copy(struct presenter *p, uint8_t *pixels)
{
    const struct rdp_rect *rect = &p->readback.rect;
    size_t row_bytes = (size_t)rect->width * 4, stride = (size_t)p->width * 4;
    const uint8_t *src;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo);
    src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)(row_bytes * (size_t)rect->height),
                           GL_MAP_READ_BIT);
    if (!src) {
        fprintf(stderr, "presenter: mapping the readback buffer failed\n");
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
readback_poll(struct presenter *p)
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
        fprintf(stderr, "presenter: the readback did not finish within a second\n");
        frame_failed(p);
        return;
    }

    /* The screen changed size under the readback; all of the new one is
     * owed already. */
    if (rb->width != p->width || rb->height != p->height) {
        if (rb->to_client)
            rdp_server_cancel_frame(p->rdp, &rb->frame);
        return;
    }

    /* The client may have left, or been resized, while the pixels were on
     * their way. Whoever comes next asks for the whole screen anyway. */
    pixels = frame_pixels(p);
    if (!pixels)
        return;

    if (readback_copy(p, pixels))
        frame_done(p, pixels);
    else
        frame_failed(p);
}

/* A dumb-buffer frame (a compositor rendering without the GPU): copy all of
 * it out of the kernel. XRGB8888 is B, G, R, X in memory, as wanted. */
static bool
read_dumb(struct presenter *p, uint8_t *pixels)
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
 * Fetch what DXGDRM_GET_FRAME has to say. Only called when the node polls
 * readable, so it does not wait. Returns false if the node is gone.
 */
static bool
fetch_frame(struct presenter *p)
{
    struct drm_dxgdrm_get_frame frame = { .seq = p->seq, .timeout_ms = 1 };
    bool resized;

    if (ioctl(p->drm_fd, DRM_IOCTL_DXGDRM_GET_FRAME, &frame)) {
        if (errno == ETIME || errno == EINTR)
            return true;
        fprintf(stderr, "presenter: DXGDRM_GET_FRAME failed: %s\n", strerror(errno));
        return false;
    }
    p->seq = frame.seq;

    /* The cursor plane is the client's to draw; all that happens here is
     * saying when its image changes. Moves alone are not reported. */
    if (frame.cursor_fd >= 0)
        close(frame.cursor_fd);
    if (!(frame.flags & DXGDRM_FRAME_CURSOR)) {
        if (p->have_cursor)
            fprintf(stderr, "presenter: cursor hidden\n");
        p->have_cursor = false;
    } else if (!p->have_cursor || frame.cursor_seq != p->cursor_seq) {
        fprintf(stderr, "presenter: cursor updated: %ux%u %s buffer %llu, hotspot %d,%d, at %d,%d\n",
                frame.cursor_width, frame.cursor_height,
                (frame.flags & DXGDRM_FRAME_CURSOR_SHARED) ? "d3d12" : "dumb",
                (unsigned long long)frame.cursor_buffer_id,
                frame.cursor_hot_x, frame.cursor_hot_y, frame.cursor_x, frame.cursor_y);
        p->have_cursor = true;
        p->cursor_seq = frame.cursor_seq;
    }

    /* A compositor leaves its last frame up when it goes away. */
    if (!!(frame.flags & DXGDRM_FRAME_OWNED) != p->owned) {
        p->owned = !p->owned;
        fprintf(stderr, p->owned ? "presenter: a compositor took over the display\n"
                                 : "presenter: no compositor owns the display any more\n");
        /* Once it has had time to come up; see CLICK_DELAY_MS. */
        atomic_store(&click_at_ms, p->owned ? (long)now_ms() + CLICK_DELAY_MS : 0);
    }

    if (!(frame.flags & DXGDRM_FRAME_PRIMARY)) {
        if (p->have_frame)
            fprintf(stderr, "presenter: the compositor turned the display off\n");
        p->have_frame = false;
        p->pending = false;
        return true;
    }

    resized = (int)frame.width != p->width || (int)frame.height != p->height;
    p->width = (int)frame.width;
    p->height = (int)frame.height;
    p->buffer_id = frame.buffer_id;
    p->dumb = !(frame.flags & DXGDRM_FRAME_SHARED);
    if (p->rdp)
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
 * Start on the damage that is owed, if there is somewhere to put it.
 *
 * One readback covers the bounding box of the damage, as mutter's RDP backend
 * does it; the wire carries a single rect anyway. The driver only takes the
 * direct path when a row is a multiple of 256 bytes (D3D12's placed-footprint
 * pitch), so the box is widened to a multiple of 64 pixels.
 */
static void
try_present(struct presenter *p)
{
    struct readback *rb = &p->readback;
    enum rdp_state state = p->rdp ? rdp_server_state(p->rdp) : RDP_NO_CLIENT;
    struct rdp_rect rect;
    uint8_t *pixels;

    if (!p->have_frame || rb->active)
        return;

    if (p->rdp && rdp_server_take_full_request(p->rdp))
        add_full_damage(p);

    if (state == RDP_BUSY || !p->pending)
        return;

    /* Nobody to show it to. A client that connects gets the whole screen. */
    if (state == RDP_NO_CLIENT && !p->out_dir) {
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

    rb->to_client = state == RDP_READY;
    if (rb->to_client) {
        if (!rdp_server_begin_frame(p->rdp, &rect, &rb->frame))
            return;
    } else if (!resize_shadow(p)) {
        return;
    }

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
    pixels = frame_pixels(p);
    if (pixels && read_dumb(p, pixels))
        frame_done(p, pixels);
    else
        frame_failed(p);
}

/*
 * Input nobody typed: the click that gets GNOME out of its overview, and with
 * --circle a pointer going round, which gives the compositor something to
 * repaint when there is no client to move the real one.
 */
struct pointer_thread {
    struct input *input;
    bool circle;
};

static void *
pointer_thread(void *data)
{
    const struct pointer_thread *args = data;
    const int range = 65536;
    double angle = 0.0;

    while (!quit) {
        long click_at = atomic_load(&click_at_ms);

        if (click_at && now_ms() >= (double)click_at) {
            atomic_store(&click_at_ms, 0);
            input_pointer_motion(args->input, range / 2, range / 2, range, range);
            input_pointer_button(args->input, BTN_LEFT, true);
            usleep(50000);
            input_pointer_button(args->input, BTN_LEFT, false);
            fprintf(stderr, "presenter: clicked in the middle of the screen\n");
        }

        if (args->circle) {
            /* Around the centre, a quarter of the height in radius, once
             * every four seconds. */
            double x = 0.5 + 0.25 * cos(angle) * 9.0 / 16.0;
            double y = 0.5 + 0.25 * sin(angle);

            input_pointer_motion(args->input, (int)(x * range), (int)(y * range), range, range);
            angle += 2.0 * M_PI / (4.0 * 60.0);
        }

        usleep(1000000 / 60);
    }
    return NULL;
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
            "  --no-rdp        no RDP server; only useful with --out\n"
            "  --out DIR       also write every frame to DIR as a JPEG\n"
            "  --max-frames N  keep N files, then start over at frame-000000 (default 600)\n"
            "  --quality Q     JPEG quality (default 85)\n"
            "  --no-input      create no uinput devices: no input from the client, and no\n"
            "                  click in the middle of the screen three seconds after a\n"
            "                  compositor takes the display (which leaves GNOME's overview)\n"
            "  --circle        move the pointer in a circle\n"
            "  --verbose       a line for every frame\n",
            argv0);
}

int
main(int argc, char **argv)
{
    struct presenter p = {
        .max_frames = 600,
        .quality = 85,
    };
    struct rdp_config rdp_config = {
        .vsock_port = 3389,
        .shm_dir = "/mnt/wslg-shared-memory",
    };
    struct pointer_thread pointer_args = { 0 };
    struct sigaction action = { .sa_handler = on_signal };
    bool use_rdp = true, use_input = true, have_pointer_thread = false;
    pthread_t pointer_tid;
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
        } else if (!strcmp(argv[i], "--no-rdp")) {
            use_rdp = false;
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            p.out_dir = argv[++i];
        } else if (!strcmp(argv[i], "--max-frames") && i + 1 < argc) {
            p.max_frames = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--quality") && i + 1 < argc) {
            p.quality = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--no-input")) {
            use_input = false;
        } else if (!strcmp(argv[i], "--circle")) {
            pointer_args.circle = true;
        } else if (!strcmp(argv[i], "--verbose")) {
            p.verbose = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!p.max_frames)
        p.max_frames = 1;
    if (rdp_config.vsock_port <= 0 || rdp_config.tcp_port < 0 || rdp_config.tcp_port > 65535) {
        usage(argv[0]);
        return 2;
    }

    /* No SA_RESTART: a signal has to get us out of poll(). A client that goes
     * away mid-write must not take the presenter with it. */
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (p.out_dir && mkdir(p.out_dir, 0755) && errno != EEXIST) {
        fprintf(stderr, "presenter: cannot create %s: %s\n", p.out_dir, strerror(errno));
        return 1;
    }

    p.drm_fd = open_dxgdrm();
    if (p.drm_fd < 0 || !init_egl(&p))
        return 1;

    if (use_input)
        p.input = input_new();

    if (use_rdp) {
        rdp_config.input = p.input;
        rdp_config.verbose = p.verbose;
        p.rdp = rdp_server_new(&rdp_config);
        if (!p.rdp)
            return 1;
    }

    if (p.input) {
        pointer_args.input = p.input;
        have_pointer_thread = !pthread_create(&pointer_tid, NULL, pointer_thread, &pointer_args);
    }

    /* The first call is what makes the node poll readable from then on. It
     * returns at once if a compositor is already up. */
    if (!fetch_frame(&p))
        return 1;
    if (!p.have_frame)
        fprintf(stderr, "presenter: waiting for the compositor's first commit\n");
    p.stat_since = now_ms();

    while (!quit) {
        struct pollfd fds[1 + 80];
        int n = 1, rdp_n = 0, timeout = 1000, rdp_timeout;

        fds[0] = (struct pollfd){ .fd = p.drm_fd, .events = POLLIN };
        if (p.rdp) {
            rdp_n = rdp_server_get_fds(p.rdp, &fds[1], 80);
            n += rdp_n;
            rdp_timeout = rdp_server_timeout_ms(p.rdp);
            if (rdp_timeout >= 0 && rdp_timeout < timeout)
                timeout = rdp_timeout;
        }
        /* GLib rounds its timeouts to a millisecond too; the fence of a
         * damage-sized readback takes about two. */
        if (p.readback.active)
            timeout = 1;

        if (poll(fds, (nfds_t)n, timeout) < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "presenter: poll failed: %s\n", strerror(errno));
            break;
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "presenter: the dxgdrm node went away\n");
            break;
        }
        if ((fds[0].revents & POLLIN) && !fetch_frame(&p))
            break;

        if (p.rdp)
            rdp_server_dispatch(p.rdp, &fds[1], rdp_n);

        readback_poll(&p);
        try_present(&p);

        if (now_ms() - p.stat_since >= 5000.0) {
            if (p.stat_frames && !p.verbose)
                fprintf(stderr, "presenter: %u frame(s) in %.0f s, %.2f ms and %.0f%% of the "
                                "screen each on average\n",
                        p.stat_frames, (now_ms() - p.stat_since) / 1000.0,
                        p.stat_ms / p.stat_frames,
                        100.0 * p.stat_pixels / p.stat_frames /
                            ((double)p.width * p.height > 0 ? (double)p.width * p.height : 1.0));
            p.stat_frames = 0;
            p.stat_ms = p.stat_pixels = 0.0;
            p.stat_since = now_ms();
        }
    }

    quit = 1;
    if (have_pointer_thread)
        pthread_join(pointer_tid, NULL);

    if (p.readback.active)
        glDeleteSync(p.readback.sync);
    rdp_server_free(p.rdp);
    input_free(p.input);
    for (int i = 0; i < MAX_IMPORTS; i++)
        destroy_import(&p, &p.imports[i]);
    free(p.shadow);
    fprintf(stderr, "presenter: %u frame(s)\n", p.frames);
    return 0;
}
