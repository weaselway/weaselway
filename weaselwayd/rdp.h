/*
 * weaselwayd's RDP side: a FreeRDP 3 server on a vsock that hands the
 * Windows client the screen through gfxredir shared memory, and puts the
 * client's mouse and keyboard on the uinput devices.
 *
 * Ported from mutter's src/backends/rdp/meta-rdp-server.c. Everything here
 * runs on the thread that calls rdp_server_dispatch(), except the gfxredir
 * channel's own reader thread, which only leaves notes for it.
 */

#ifndef WEASELWAY_RDP_H
#define WEASELWAY_RDP_H

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

struct input;
struct rdp_server;

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
    bool verbose;
};

struct rdp_rect {
    int x, y, width, height;
};

/* A buffer in the client's pool that a frame is being written to. */
struct rdp_frame {
    uint64_t generation;
    int index;
};

enum rdp_state {
    /* Nobody to present to. */
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

struct rdp_server *rdp_server_new(const struct rdp_config *config);
void rdp_server_free(struct rdp_server *server);

/* The descriptors to poll for reading; returns how many were filled in. */
int rdp_server_get_fds(struct rdp_server *server, struct pollfd *fds, int max);
/* How long poll() may sleep at most, in ms; -1 for no limit. */
int rdp_server_timeout_ms(struct rdp_server *server);
/* Call after every poll(), with the array rdp_server_get_fds() filled. */
void rdp_server_dispatch(struct rdp_server *server, const struct pollfd *fds, int n);

/* The size of the screen being presented; 0x0 while there is none. */
void rdp_server_set_screen_size(struct rdp_server *server, int width, int height);

/* True, once, when the client wants the screen to have another size: the one
 * it connected with, or the one its window was resized to. The screen is
 * given a few seconds to follow; a client whose desktop still differs from it
 * after that is resized to the screen instead. */
bool rdp_server_take_size_request(struct rdp_server *server, int *width, int *height);

enum rdp_state rdp_server_state(struct rdp_server *server);

/* True, once, when the client needs the whole screen rather than the damage:
 * it has just connected, or come back from a resize. */
bool rdp_server_take_full_request(struct rdp_server *server);

/*
 * What the client's mouse pointer looks like: @width x @height pixels, B G R A
 * premultiplied, top row first, @stride bytes a row, with the hotspot inside.
 * NULL @pixels hides it. The client draws it at its own mouse position, so
 * moves need nothing from here. Kept for a client that connects later.
 */
void rdp_server_set_pointer(struct rdp_server *server, const uint8_t *pixels, int stride,
                            int width, int height, int hot_x, int hot_y);

/*
 * Writing a frame, in three steps so that the pixels can arrive later:
 *
 *   begin   picks a buffer the client is not reading and brings it up to date
 *           everywhere except @rect. Only in state RDP_READY; until the frame
 *           is ended or cancelled the state is RDP_BUSY.
 *   pixels  the buffer, width * 4 bytes a row, B G R X. NULL if the pool went
 *           away in the meantime (the client left or the screen was resized);
 *           the frame is then over. Valid until the next dispatch.
 *   end     tells the client to show @rect of it.
 */
bool rdp_server_begin_frame(struct rdp_server *server, const struct rdp_rect *rect,
                            struct rdp_frame *frame);
uint8_t *rdp_server_frame_pixels(struct rdp_server *server, const struct rdp_frame *frame);
void rdp_server_end_frame(struct rdp_server *server, const struct rdp_frame *frame,
                          const struct rdp_rect *rect);
void rdp_server_cancel_frame(struct rdp_server *server, const struct rdp_frame *frame);

#endif
