/*
 * weaselwayd's RDP side: a FreeRDP 3 server on a vsock that hands the
 * Windows client the screens through gfxredir shared memory, and puts the
 * client's mouse and keyboard on the uinput devices.
 *
 * Every monitor of the client's is an output: output i shows monitor i of the
 * client's layout, in a gfxredir window and pool of its own.
 *
 * Ported from mutter's src/backends/rdp/meta-rdp-server.c. The server is a
 * source of the main context it was created under, and everything here runs
 * on that context's thread, except the channels' own reader threads, which
 * only leave notes for it.
 */

#ifndef WEASELWAY_RDP_H
#define WEASELWAY_RDP_H

#include <stdbool.h>
#include <stdint.h>

#include <glib.h>

#include "layout.h"

#define RDP_MAX_OUTPUTS LAYOUT_MAX_MONITORS

struct input;
struct rdp_server;
struct selection;
struct wayland_screens;

struct rdp_config {
    /* The vsock port to listen on. */
    int vsock_port;
    /* For debugging without a vsock: listen on 127.0.0.1:tcp_port instead.
     * There is no authentication on either. */
    int tcp_port;
    /* The virtio-fs/DAX share the client maps the frames from. */
    const char *shm_dir;
    /* Where the client's input goes; NULL for none. */
    struct input *input;
    /* The session's clipboard, which the client's is kept in step with; NULL
     * for none. */
    struct selection *selection;
    /* Where the compositor put the outputs, which the pointer is mapped onto;
     * NULL, or while it has not said, the client's layout is used. */
    struct wayland_screens *screens;
    bool verbose;
    /* Called whenever the server has dispatched: the client's state, the
     * size it wants, or whether it wants the whole screen may be another. */
    void (*changed)(void *data);
    void *changed_data;
};

struct rdp_rect {
    int x, y, width, height;
};

/* Grow @into to the bounding box of itself and @rect. */
void rdp_rect_union(struct rdp_rect *into, const struct rdp_rect *rect);

/* A buffer in an output's pool that a frame is being written to. */
struct rdp_frame {
    uint64_t generation;
    int output;
    int index;
};

enum rdp_state {
    /* Nobody to present to: no client, one whose window is minimised, or one
     * with fewer monitors than the output's number. Damage need not be kept;
     * whoever looks next asks for the whole screen. */
    RDP_NO_CLIENT,
    /* A client is there but not ready for frames yet: still connecting, or
     * being resized. Damage has to be kept. */
    RDP_CONNECTING,
    /* The client cannot take a frame right now: it holds every buffer, or one
     * is being written. Damage has to be kept, and the compositor should
     * wait. */
    RDP_BUSY,
    RDP_READY,
};

/* Starts listening, on the thread-default main context. */
struct rdp_server *rdp_server_new(const struct rdp_config *config, GError **error);
void rdp_server_free(struct rdp_server *server);

/* The size of the screen @output is presenting; 0x0 while there is none. */
void rdp_server_set_screen_size(struct rdp_server *server, int output, int width, int height);

/* True, once, when the client wants other screens: the monitors it connected
 * with, or the ones its windows were resized to. @monitors[i] is what output
 * i should be, positioned so the layout starts at 0,0. The screens are given
 * a few seconds to follow; a client whose desktop still differs from them
 * after that is resized to them instead. Also true when only the refresh rate
 * of a monitor changed; a monitor's refresh_mhz is 0 until the client has
 * said. */
bool rdp_server_take_layout_request(struct rdp_server *server,
                                    struct monitor monitors[RDP_MAX_OUTPUTS], int *count);

/* True, once, when the client has said where the last @frames frames of
 * @output were ready relative to its monitor's vblank: @slack_us before it,
 * the 10th percentile. */
bool rdp_server_take_present_timing(struct rdp_server *server, int output, uint32_t *frames,
                                    int32_t *slack_us);

/* The size the connected client wants @output's screen to have; false while
 * there is no client yet, or it has no such monitor. For a screen the
 * compositor does not give a size to. */
bool rdp_server_client_size(struct rdp_server *server, int output, int *width, int *height);

enum rdp_state rdp_server_state(struct rdp_server *server, int output);

/* True, once, when the client needs the whole of @output's screen rather than
 * the damage: it has just connected, come back from a resize, or shows its
 * windows again. */
bool rdp_server_take_full_request(struct rdp_server *server, int output);

/*
 * What the client's mouse pointer looks like: @width x @height pixels, B G R A
 * premultiplied, top row first, @stride bytes a row, with the hotspot inside.
 * NULL @pixels hides it. The client draws it at its own mouse position, so
 * moves need nothing from here. One for all outputs. Kept for a client that
 * connects later.
 */
void rdp_server_set_pointer(struct rdp_server *server, const uint8_t *pixels, int stride,
                            int width, int height, int hot_x, int hot_y);

/*
 * Writing a frame, in three steps so that the pixels can arrive later:
 *
 *   begin   picks a buffer the client is not reading and brings it up to date
 *           everywhere except @rect. Only in state RDP_READY; until the frame
 *           is ended or cancelled the state is RDP_BUSY.
 *   pixels  the buffer, @stride bytes a row, B G R X. NULL if the pool went
 *           away in the meantime (the client left or the screen was resized);
 *           the frame is then over. Valid until the main loop runs again.
 *   end     tells the client to show @rect of it.
 */
bool rdp_server_begin_frame(struct rdp_server *server, int output, const struct rdp_rect *rect,
                            struct rdp_frame *frame);
uint8_t *rdp_server_frame_pixels(struct rdp_server *server, const struct rdp_frame *frame,
                                 size_t *stride);
void rdp_server_end_frame(struct rdp_server *server, const struct rdp_frame *frame,
                          const struct rdp_rect *rect);
void rdp_server_cancel_frame(struct rdp_server *server, const struct rdp_frame *frame);

#endif
