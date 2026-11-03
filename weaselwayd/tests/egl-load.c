/*
 * A load for the frame path: a fullscreen Wayland window that redraws every
 * pixel every frame with EGL and GLES2, paced by the compositor. Prints the
 * frame rate every two seconds.
 *
 *   egl-load [seconds]    run that long, 0 or nothing for ever
 */

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "xdg-shell-client-protocol.h"

struct app {
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct wl_egl_window *egl_window;
    int width, height;
    bool configured, closed;
};

static double
now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void
wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                uint32_t version)
{
    struct app *app = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
    }
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void
surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    struct app *app = data;

    xdg_surface_ack_configure(surface, serial);
    app->configured = true;
}

static const struct xdg_surface_listener surface_listener = { .configure = surface_configure };

static void
toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height,
                   struct wl_array *states)
{
    struct app *app = data;

    /* 0 leaves the size to us; fullscreen always gives one. */
    if (width > 0 && height > 0) {
        app->width = width;
        app->height = height;
        if (app->egl_window)
            wl_egl_window_resize(app->egl_window, width, height, 0, 0);
    }
}

static void
toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    struct app *app = data;

    app->closed = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static const char *vertex_source = "attribute vec2 pos;\n"
                                   "varying vec2 uv;\n"
                                   "void main() {\n"
                                   "  uv = pos * 0.5 + 0.5;\n"
                                   "  gl_Position = vec4(pos, 0.0, 1.0);\n"
                                   "}\n";

/* A plasma that moves everywhere at once, so every pixel changes every frame
 * and the damage is the whole screen. */
static const char *fragment_source =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform float t;\n"
    "void main() {\n"
    "  vec2 p = uv * 8.0;\n"
    "  float v = sin(p.x + t * 3.0) + sin(p.y * 1.3 - t * 2.0) +\n"
    "            sin(length(p - vec2(4.0)) * 1.7 - t * 4.0);\n"
    "  gl_FragColor = vec4(0.5 + 0.5 * sin(v + t), 0.5 + 0.5 * sin(v * 1.3 + 2.0),\n"
    "                      0.5 + 0.5 * sin(v * 0.7 + 4.0 - t), 1.0);\n"
    "}\n";

static GLuint
shader(GLenum type, const char *source)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];

        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "egl-load: shader: %s\n", log);
        exit(1);
    }
    return s;
}

int
main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 0.0;
    struct app app = { .width = 1280, .height = 720 };
    struct wl_display *display = wl_display_connect(NULL);

    if (!display) {
        fprintf(stderr, "egl-load: no Wayland display\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &app);
    wl_display_roundtrip(display);
    if (!app.compositor || !app.wm_base) {
        fprintf(stderr, "egl-load: no wl_compositor or xdg_wm_base\n");
        return 1;
    }

    struct wl_surface *surface = wl_compositor_create_surface(app.compositor);
    struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &surface_listener, &app);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, &app);
    xdg_toplevel_set_title(toplevel, "egl-load");
    xdg_toplevel_set_fullscreen(toplevel, NULL);
    wl_surface_commit(surface);
    while (!app.configured && wl_display_dispatch(display) >= 0)
        ;

    EGLDisplay egl = eglGetDisplay((EGLNativeDisplayType)display);
    EGLint major, minor, count;
    EGLConfig config;
    static const EGLint config_attribs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8,
                                             EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                             EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    if (!eglInitialize(egl, &major, &minor) ||
        !eglChooseConfig(egl, config_attribs, &config, 1, &count) || count < 1) {
        fprintf(stderr, "egl-load: no EGL config\n");
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLContext context = eglCreateContext(egl, config, EGL_NO_CONTEXT, context_attribs);
    app.egl_window = wl_egl_window_create(surface, app.width, app.height);
    EGLSurface egl_surface =
        eglCreateWindowSurface(egl, config, (EGLNativeWindowType)app.egl_window, NULL);
    if (context == EGL_NO_CONTEXT || egl_surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(egl, egl_surface, egl_surface, context)) {
        fprintf(stderr, "egl-load: EGL setup failed (0x%x)\n", eglGetError());
        return 1;
    }
    /* Waits for the compositor's frame callback: one frame per refresh. */
    eglSwapInterval(egl, 1);
    fprintf(stderr, "egl-load: %s\n", (const char *)glGetString(GL_RENDERER));

    GLuint program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER, vertex_source));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER, fragment_source));
    glBindAttribLocation(program, 0, "pos");
    glLinkProgram(program);
    glUseProgram(program);
    GLint t_location = glGetUniformLocation(program, "t");
    static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);

    double start = now(), report = start;
    unsigned frames = 0;

    while (!app.closed && wl_display_dispatch_pending(display) >= 0) {
        double t = now();

        if (seconds > 0.0 && t - start >= seconds)
            break;
        glViewport(0, 0, app.width, app.height);
        glUniform1f(t_location, (GLfloat)(t - start));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (!eglSwapBuffers(egl, egl_surface)) {
            fprintf(stderr, "egl-load: eglSwapBuffers failed (0x%x)\n", eglGetError());
            return 1;
        }
        frames++;
        if (t - report >= 2.0) {
            fprintf(stderr, "egl-load: %dx%d %.1f fps\n", app.width, app.height,
                    frames / (t - report));
            frames = 0;
            report = t;
        }
    }

    eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl, egl_surface);
    eglDestroyContext(egl, context);
    wl_egl_window_destroy(app.egl_window);
    eglTerminate(egl);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surface);
    wl_surface_destroy(surface);
    wl_display_disconnect(display);
    return 0;
}
