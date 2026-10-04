/*
 * The presenter's input devices: an absolute pointer and a keyboard on uinput.
 * To the compositor they are ordinary evdev devices that libinput picks up.
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

/* Let go of everything still held, for when the client goes away. */
void input_release_all(struct input *input);

#endif
