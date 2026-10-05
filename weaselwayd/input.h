/*
 * weaselwayd's input devices: an absolute pointer, a keyboard and a
 * multitouch touchpad on uinput. To the compositor they are ordinary evdev
 * devices that libinput picks up.
 */

#ifndef WEASELWAY_INPUT_H
#define WEASELWAY_INPUT_H

#include <stdbool.h>
#include <stdint.h>

struct input;

/* NULL if /dev/uinput cannot be used; the session then runs without input. */
struct input *input_new(void);
void input_free(struct input *input);

/* A position in pixels on a screen of the given size. */
void input_pointer_motion(struct input *input, int x, int y, int width, int height);
/* An evdev button code (BTN_LEFT, ...). */
void input_pointer_button(struct input *input, uint16_t button, bool pressed);
/* Wheel travel in 1/120ths of a notch; positive is away from the user, or to
 * the right, as in both evdev and RDP. */
void input_pointer_wheel(struct input *input, int value120, bool horizontal);
/* An evdev key code (KEY_A, ...). */
void input_key(struct input *input, uint16_t key, bool pressed);
/* Which locks are on at the client's keyboard. One that is not in the same
 * state in the session gets its key tapped. */
void input_sync_locks(struct input *input, bool num, bool caps, bool scroll);

/*
 * The touchpad gets the fingers as they are on the client's pad, and libinput
 * makes swipes and pinches of them like of any other touchpad's. A contact is
 * named by the client's id for it; @x and @y are its place on the pad, 0 to 1.
 * What changed takes effect with the frame that follows, so fingers that move
 * together are seen to.
 */
void input_touchpad_contact(struct input *input, uint32_t id, bool down, double x, double y);
void input_touchpad_frame(struct input *input);
/* Lift every finger, for a client that stopped saying where they are. */
void input_touchpad_release(struct input *input);

/* Let go of everything still held, for when the client goes away. */
void input_release_all(struct input *input);

#endif
