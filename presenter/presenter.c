/*
 * weaselway-presenter: the userspace half of dxgdrm's virtual display.
 *
 * Spike. The compositor scans out to dxgdrm's KMS node; this waits for each
 * commit (DXGDRM_GET_FRAME), reads the frame back and writes it out as a JPEG
 * instead of sending it to Windows. It answers the three open questions:
 *
 *   - can a second process open the compositor's scanout buffer on its own
 *     device and read back only what was damaged, and how long does that take
 *     (the timings it prints);
 *   - does the compositor take input from a uinput device (it moves a pointer
 *     in a circle, which shows up in the frames);
 *   - does any of this work with an unmodified compositor at all.
 *
 * The readback goes through GL rather than D3D12 directly: the frame is a
 * D3D12 shared handle, Mesa's d3d12 driver imports one as a dma-buf
 * (OpenSharedHandle underneath), and glReadPixels on it is the same
 * CopyTextureRegion + map the real presenter will do.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/uinput.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <jpeglib.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#include "dxgdrm_drm.h"

#define MAX_IMPORTS 8

/* One imported scanout buffer. A compositor flips between a handful. */
struct import {
    uint64_t buffer_id;
    uint64_t last_used;
    EGLImage image;
    GLuint texture;
    GLuint fbo;
};

struct presenter {
    int drm_fd;
    struct gbm_device *gbm;
    EGLDisplay display;
    EGLContext context;

    struct import imports[MAX_IMPORTS];
    uint64_t use_counter;

    /* The screen as last read back, RGBX, top row first. */
    uint8_t *shadow;
    uint32_t width, height;

    /* The cursor plane's image, ARGB8888 as the compositor wrote it. */
    uint8_t *cursor;
    uint32_t cursor_width, cursor_height, cursor_pitch;
    uint64_t cursor_seq;

    uint8_t *compose;

    /* Asynchronous readback: the pixel buffer glReadPixels targets, and what
     * the last one cost, for the frame line. */
    GLuint pbo;
    size_t pbo_size;
    bool sync_readback;
    bool always_full;
    double issue_ms, wait_ms, copy_ms;

    const char *out_dir;
    unsigned max_frames;
    int quality;
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

    /* The spike is only worth anything on the GPU: llvmpipe cannot open a
     * D3D12 shared handle. */
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
get_import(struct presenter *p, const struct drm_dxgdrm_get_frame *frame)
{
    struct import *import = NULL, *oldest = &p->imports[0];
    EGLAttrib attribs[] = {
        EGL_WIDTH, (EGLAttrib)frame->width,
        EGL_HEIGHT, (EGLAttrib)frame->height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLAttrib)frame->format,
        EGL_DMA_BUF_PLANE0_FD_EXT, frame->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLAttrib)frame->pitch,
        EGL_NONE,
    };

    for (int i = 0; i < MAX_IMPORTS; i++) {
        if (p->imports[i].buffer_id == frame->buffer_id) {
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
                    (unsigned long long)frame->buffer_id, eglGetError());
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
                    (unsigned long long)frame->buffer_id);
            import->buffer_id = frame->buffer_id;
            destroy_import(p, import);
            return NULL;
        }

        import->buffer_id = frame->buffer_id;
        fprintf(stderr, "presenter: imported buffer %llu (%ux%u, %.4s)\n",
                (unsigned long long)frame->buffer_id, frame->width, frame->height,
                (const char *)&frame->format);
    }

    import->last_used = ++p->use_counter;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, import->fbo);
    return import;
}

static bool
resize_shadow(struct presenter *p, uint32_t width, uint32_t height)
{
    if (p->shadow && p->width == width && p->height == height)
        return true;

    free(p->shadow);
    free(p->compose);
    p->shadow = calloc((size_t)width * height, 4);
    p->compose = malloc((size_t)width * height * 4);
    p->width = width;
    p->height = height;
    return p->shadow && p->compose;
}

/* Read the damaged part of a shared-handle frame into the shadow copy.
 * Returns the number of pixels read, or -1. */
static long
read_shared(struct presenter *p, const struct drm_dxgdrm_get_frame *frame)
{
    struct drm_dxgdrm_rect full = { 0, 0, (int32_t)frame->width, (int32_t)frame->height };
    const struct drm_dxgdrm_rect *rects = frame->damage;
    unsigned num_rects = frame->num_damage;
    long pixels = 0;

    if (!get_import(p, frame))
        return -1;

    if (frame->flags & DXGDRM_FRAME_DAMAGE_FULL) {
        rects = &full;
        num_rects = 1;
    }

    /* A dma-buf's first row is the top one, and GL calls the first row y = 0,
     * so nothing is flipped: rows land in the shadow copy top down. */
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)frame->width);

    for (unsigned i = 0; i < num_rects; i++) {
        int x1 = rects[i].x1 < 0 ? 0 : rects[i].x1;
        int y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
        int x2 = rects[i].x2 > (int)frame->width ? (int)frame->width : rects[i].x2;
        int y2 = rects[i].y2 > (int)frame->height ? (int)frame->height : rects[i].y2;

        if (x2 <= x1 || y2 <= y1)
            continue;

        glReadPixels(x1, y1, x2 - x1, y2 - y1, GL_RGBA, GL_UNSIGNED_BYTE,
                     p->shadow + ((size_t)y1 * frame->width + x1) * 4);
        pixels += (long)(x2 - x1) * (y2 - y1);
    }

    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "presenter: glReadPixels failed\n");
        return -1;
    }

    return pixels;
}

/*
 * The same, without the presenter's thread ever waiting inside glReadPixels.
 *
 * Reading into a client pointer makes the driver copy to a staging buffer,
 * wait for the GPU, map and memcpy before it returns. Reading into a pixel
 * buffer object does not have to: Mesa's d3d12 driver (weaselway's) issues a
 * texture -> buffer copy and returns. A fence says when the copy has landed,
 * and the map after that does not block.
 *
 * One readback covers the bounding box of the damage, as mutter's RDP backend
 * does it. The driver only takes the direct path when a row is a multiple of
 * 256 bytes (D3D12's placed-footprint pitch), so the box is widened to a
 * multiple of 64 pixels.
 *
 * This stub still waits for the fence before it does anything else, so a frame
 * takes as long as before end to end. What it shows is where the time goes
 * (issue / wait / copy): the wait is the part a real presenter spends in
 * poll() on the fence, next to its sockets, instead of inside the driver.
 */
static long
read_shared_async(struct presenter *p, const struct drm_dxgdrm_get_frame *frame)
{
    int width = (int)frame->width, height = (int)frame->height;
    int x1 = width, y1 = height, x2 = 0, y2 = 0, w, h;
    size_t full_size = (size_t)width * height * 4;
    const uint8_t *src;
    GLsync sync;
    GLenum status;
    double t0, t1, t2;

    if (!get_import(p, frame))
        return -1;

    if (frame->flags & DXGDRM_FRAME_DAMAGE_FULL) {
        x1 = y1 = 0;
        x2 = width;
        y2 = height;
    } else {
        for (unsigned i = 0; i < frame->num_damage; i++) {
            const struct drm_dxgdrm_rect *r = &frame->damage[i];

            if (r->x1 < x1) x1 = r->x1;
            if (r->y1 < y1) y1 = r->y1;
            if (r->x2 > x2) x2 = r->x2;
            if (r->y2 > y2) y2 = r->y2;
        }
    }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > width) x2 = width;
    if (y2 > height) y2 = height;
    if (x2 <= x1 || y2 <= y1)
        return 0;

    /* Widen to a multiple of 64 pixels, moving left where the right edge is
     * in the way. A frame narrower than that takes the driver's slow path. */
    w = (x2 - x1 + 63) & ~63;
    if (w > width)
        w = width;
    if (x1 + w > width)
        x1 = width - w;
    h = y2 - y1;

    t0 = now_ms();

    if (!p->pbo)
        glGenBuffers(1, &p->pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo);
    /* Sized for a whole frame once, so that no damage shape reallocates it. */
    if (p->pbo_size != full_size) {
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)full_size, NULL, GL_STREAM_READ);
        p->pbo_size = full_size;
    }

    /* Tightly packed at offset 0: the transfer is exactly the box. */
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(x1, y1, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    t1 = now_ms();

    status = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    glDeleteSync(sync);
    t2 = now_ms();
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
        fprintf(stderr, "presenter: the readback did not finish within a second\n");
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return -1;
    }

    src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)((size_t)w * h * 4),
                           GL_MAP_READ_BIT);
    if (!src) {
        fprintf(stderr, "presenter: mapping the readback buffer failed\n");
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return -1;
    }

    for (int y = 0; y < h; y++)
        memcpy(p->shadow + ((size_t)(y1 + y) * width + x1) * 4,
               src + (size_t)y * w * 4, (size_t)w * 4);

    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    p->issue_ms = t1 - t0;
    p->wait_ms = t2 - t1;
    p->copy_ms = now_ms() - t2;
    return (long)w * h;
}

/* A dumb-buffer frame (a compositor rendering without the GPU): copy all of
 * it out of the kernel. */
static long
read_dumb(struct presenter *p, const struct drm_dxgdrm_get_frame *frame)
{
    size_t size = (size_t)frame->pitch * frame->height;
    struct drm_dxgdrm_read_pixels read = {
        .plane = DXGDRM_PLANE_PRIMARY,
        .size = (uint32_t)size,
    };
    uint8_t *pixels = malloc(size);

    if (!pixels)
        return -1;
    read.data = (uintptr_t)pixels;

    if (drmIoctl(p->drm_fd, DRM_IOCTL_DXGDRM_READ_PIXELS, &read) ||
        read.width != frame->width || read.height != frame->height) {
        free(pixels);
        return -1;
    }

    /* XRGB8888 is B, G, R, X in memory. */
    for (uint32_t y = 0; y < read.height; y++) {
        const uint8_t *src = pixels + (size_t)y * read.pitch;
        uint8_t *dst = p->shadow + (size_t)y * read.width * 4;

        for (uint32_t x = 0; x < read.width; x++, src += 4, dst += 4) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 0xff;
        }
    }

    free(pixels);
    return (long)read.width * read.height;
}

static void
read_cursor(struct presenter *p, const struct drm_dxgdrm_get_frame *frame)
{
    size_t size = (size_t)frame->cursor_width * frame->cursor_height * 4;
    struct drm_dxgdrm_read_pixels read = {
        .plane = DXGDRM_PLANE_CURSOR,
        .size = (uint32_t)size,
    };

    free(p->cursor);
    p->cursor = malloc(size);
    p->cursor_width = p->cursor_height = 0;
    if (!p->cursor)
        return;
    read.data = (uintptr_t)p->cursor;

    if (drmIoctl(p->drm_fd, DRM_IOCTL_DXGDRM_READ_PIXELS, &read)) {
        fprintf(stderr, "presenter: reading the cursor failed: %s\n", strerror(errno));
        return;
    }

    p->cursor_width = read.width;
    p->cursor_height = read.height;
    p->cursor_pitch = read.pitch;
}

/* Shadow copy plus the cursor plane, which is what the client would draw. */
static void
compose(struct presenter *p, const struct drm_dxgdrm_get_frame *frame)
{
    memcpy(p->compose, p->shadow, (size_t)p->width * p->height * 4);

    if (!(frame->flags & DXGDRM_FRAME_CURSOR) || !p->cursor_width)
        return;

    for (uint32_t cy = 0; cy < p->cursor_height; cy++) {
        int y = frame->cursor_y + (int)cy;

        if (y < 0 || y >= (int)p->height)
            continue;

        for (uint32_t cx = 0; cx < p->cursor_width; cx++) {
            int x = frame->cursor_x + (int)cx;
            const uint8_t *src = p->cursor + (size_t)cy * p->cursor_pitch + cx * 4;
            uint8_t *dst;
            unsigned inv;

            if (x < 0 || x >= (int)p->width || !src[3])
                continue;

            /* ARGB8888, premultiplied: B, G, R, A in memory. */
            dst = p->compose + ((size_t)y * p->width + x) * 4;
            inv = 255 - src[3];
            dst[0] = (uint8_t)(src[2] + dst[0] * inv / 255);
            dst[1] = (uint8_t)(src[1] + dst[1] * inv / 255);
            dst[2] = (uint8_t)(src[0] + dst[2] * inv / 255);
        }
    }
}

static bool
write_jpeg(struct presenter *p, unsigned index)
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
    cinfo.image_width = p->width;
    cinfo.image_height = p->height;
    cinfo.input_components = 4;
    cinfo.in_color_space = JCS_EXT_RGBX;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, p->quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = p->compose + (size_t)cinfo.next_scanline * p->width * 4;

        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(file);

    /* So a viewer following the directory never sees half a file. */
    return rename(tmp, path) == 0;
}

/*
 * Simulated input: an absolute pointer on uinput, moved in a circle. To the
 * compositor it is an ordinary evdev device that libinput picks up, which is
 * how the real presenter will deliver the Windows pointer.
 */

#define POINTER_ABS_MAX 65535

static void
emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event event = { .type = type, .code = code, .value = value };

    if (write(fd, &event, sizeof(event)) < 0) {
        /* Nothing useful to do about it; the next one will fail too. */
    }
}

static void *
pointer_thread(void *data)
{
    struct uinput_setup setup = {
        .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1d6b, .product = 0x0104 },
        .name = "Weaselway spike pointer",
    };
    struct uinput_abs_setup abs_x = { .code = ABS_X, .absinfo = { .maximum = POINTER_ABS_MAX } };
    struct uinput_abs_setup abs_y = { .code = ABS_Y, .absinfo = { .maximum = POINTER_ABS_MAX } };
    double angle = 0.0;
    int fd;

    fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "presenter: cannot open /dev/uinput (%s) -- no simulated pointer. "
                        "Is the uinput module loaded and the node writable?\n",
                strerror(errno));
        return NULL;
    }

    /* Absolute axes plus mouse buttons and nothing touch-like is what udev
     * and libinput classify as an absolute pointer, like a VM's tablet. */
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) || ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) ||
        ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) || ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE) ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) ||
        ioctl(fd, UI_ABS_SETUP, &abs_x) || ioctl(fd, UI_ABS_SETUP, &abs_y) ||
        ioctl(fd, UI_DEV_SETUP, &setup) || ioctl(fd, UI_DEV_CREATE)) {
        fprintf(stderr, "presenter: creating the uinput pointer failed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    fprintf(stderr, "presenter: uinput pointer created, circling\n");

    while (!quit) {
        /* A circle around the centre, a quarter of the height in radius,
         * once every four seconds. */
        double x = 0.5 + 0.25 * cos(angle) * 9.0 / 16.0;
        double y = 0.5 + 0.25 * sin(angle);

        emit(fd, EV_ABS, ABS_X, (int32_t)(x * POINTER_ABS_MAX));
        emit(fd, EV_ABS, ABS_Y, (int32_t)(y * POINTER_ABS_MAX));
        emit(fd, EV_SYN, SYN_REPORT, 0);

        angle += 2.0 * M_PI / (4.0 * 60.0);
        usleep(1000000 / 60);
    }

    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return NULL;
}

static void
usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--out DIR] [--max-frames N] [--quality Q] [--no-pointer] [--sync] [--full]\n"
            "  --out DIR       where the JPEGs go (default /tmp/weaselway-frames)\n"
            "  --max-frames N  keep N files, then start over at frame-000000 (default 600)\n"
            "  --quality Q     JPEG quality (default 85)\n"
            "  --no-pointer    do not create the circling uinput pointer\n"
            "  --sync          read back with a blocking glReadPixels per damage rect\n"
            "  --full          ignore the damage and read the whole frame every time\n",
            argv0);
}

int
main(int argc, char **argv)
{
    struct presenter p = {
        .out_dir = "/tmp/weaselway-frames",
        .max_frames = 600,
        .quality = 85,
    };
    struct sigaction action = { .sa_handler = on_signal };
    struct drm_dxgdrm_get_frame frame = { 0 };
    uint64_t seq = 0, primary_seq = 0;
    bool pointer = true, have_frame = false;
    pthread_t pointer_tid;
    unsigned frames = 0;
    double last_frame = 0.0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            p.out_dir = argv[++i];
        } else if (!strcmp(argv[i], "--max-frames") && i + 1 < argc) {
            p.max_frames = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--quality") && i + 1 < argc) {
            p.quality = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--no-pointer")) {
            pointer = false;
        } else if (!strcmp(argv[i], "--sync")) {
            p.sync_readback = true;
        } else if (!strcmp(argv[i], "--full")) {
            p.always_full = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!p.max_frames)
        p.max_frames = 1;

    /* No SA_RESTART: a signal has to get us out of DXGDRM_GET_FRAME. */
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    if (mkdir(p.out_dir, 0755) && errno != EEXIST) {
        fprintf(stderr, "presenter: cannot create %s: %s\n", p.out_dir, strerror(errno));
        return 1;
    }

    p.drm_fd = open_dxgdrm();
    if (p.drm_fd < 0 || !init_egl(&p))
        return 1;

    if (pointer)
        pthread_create(&pointer_tid, NULL, pointer_thread, NULL);

    fprintf(stderr, "presenter: waiting for the compositor's first commit\n");

    while (!quit) {
        double t_start, t_read, t_done, wait;
        long pixels = 0;
        bool primary_changed;

        /* The cursor moving is a commit too. One frame per display refresh
         * is plenty; damage keeps accumulating in the kernel meanwhile. */
        wait = last_frame + 1000.0 / 60.0 - now_ms();
        if (wait > 0)
            usleep((useconds_t)(wait * 1000.0));

        memset(&frame, 0, sizeof(frame));
        frame.seq = seq;
        frame.timeout_ms = 1000;
        if (ioctl(p.drm_fd, DRM_IOCTL_DXGDRM_GET_FRAME, &frame)) {
            if (errno == ETIME || errno == EINTR)
                continue;
            fprintf(stderr, "presenter: DXGDRM_GET_FRAME failed: %s\n", strerror(errno));
            break;
        }
        seq = frame.seq;
        last_frame = t_start = now_ms();

        if (!(frame.flags & DXGDRM_FRAME_PRIMARY)) {
            if (have_frame)
                fprintf(stderr, "presenter: the compositor turned the display off\n");
            have_frame = false;
            continue;
        }

        if (!resize_shadow(&p, frame.width, frame.height)) {
            fprintf(stderr, "presenter: out of memory\n");
            break;
        }

        primary_changed = frame.primary_seq != primary_seq || !have_frame;
        p.issue_ms = p.wait_ms = p.copy_ms = 0.0;
        if (primary_changed) {
            /* The shadow copy starts out empty, whatever the damage says. */
            if (!have_frame || p.always_full)
                frame.flags |= DXGDRM_FRAME_DAMAGE_FULL;

            if (!(frame.flags & DXGDRM_FRAME_SHARED))
                pixels = read_dumb(&p, &frame);
            else if (p.sync_readback)
                pixels = read_shared(&p, &frame);
            else
                pixels = read_shared_async(&p, &frame);
        }
        if (frame.fd >= 0)
            close(frame.fd);
        if (pixels < 0) {
            /* Ask for everything again next time round. */
            have_frame = false;
            continue;
        }
        primary_seq = frame.primary_seq;
        have_frame = true;
        t_read = now_ms();

        if ((frame.flags & DXGDRM_FRAME_CURSOR) &&
            (frame.cursor_seq != p.cursor_seq || !p.cursor_width)) {
            read_cursor(&p, &frame);
            p.cursor_seq = frame.cursor_seq;
        }

        compose(&p, &frame);
        if (!write_jpeg(&p, frames % p.max_frames))
            break;
        t_done = now_ms();
        frames++;

        fprintf(stderr,
                "frame %u: %s %ux%u buffer %llu, %u rect(s)%s, %.1f%% read back in %.2f ms "
                "(issue %.2f wait %.2f copy %.2f), jpeg %.2f ms, cursor %s at %d,%d\n",
                frames,
                (frame.flags & DXGDRM_FRAME_SHARED) ? "d3d12" : "dumb",
                frame.width, frame.height, (unsigned long long)frame.buffer_id,
                frame.num_damage, (frame.flags & DXGDRM_FRAME_DAMAGE_FULL) ? " (full)" : "",
                100.0 * (double)pixels / ((double)frame.width * frame.height),
                t_read - t_start, p.issue_ms, p.wait_ms, p.copy_ms, t_done - t_read,
                (frame.flags & DXGDRM_FRAME_CURSOR) ? "plane" : "none",
                frame.cursor_x, frame.cursor_y);
    }

    quit = 1;
    if (pointer)
        pthread_join(pointer_tid, NULL);

    for (int i = 0; i < MAX_IMPORTS; i++)
        destroy_import(&p, &p.imports[i]);
    fprintf(stderr, "presenter: wrote %u frame(s) to %s\n", frames, p.out_dir);
    return 0;
}
