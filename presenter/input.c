#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/uinput.h>

#define POINTER_ABS_MAX 65535

#define N_BUTTONS (BTN_TASK - BTN_LEFT + 1)

struct input {
    int pointer_fd;
    int keyboard_fd;

    /* The RDP callbacks and the simulated pointer write from different
     * threads; an event and its SYN_REPORT have to stay together. */
    pthread_mutex_t lock;

    /* What is held down, so that a repeated press is not sent twice and a
     * client that disappears does not leave a key stuck. */
    bool buttons[N_BUTTONS];
    bool keys[KEY_MAX + 1];

    /* Wheel travel that did not add up to a whole notch yet. */
    int wheel_rest[2];
};

static void
emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event event = { .type = type, .code = code, .value = value };

    if (write(fd, &event, sizeof(event)) < 0) {
        /* Nothing useful to do about it; the next one will fail too. */
    }
}

static int
create_pointer(void)
{
    struct uinput_setup setup = {
        .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1d6b, .product = 0x0104 },
        .name = "Weaselway pointer",
    };
    struct uinput_abs_setup abs_x = { .code = ABS_X, .absinfo = { .maximum = POINTER_ABS_MAX } };
    struct uinput_abs_setup abs_y = { .code = ABS_Y, .absinfo = { .maximum = POINTER_ABS_MAX } };
    int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;

    /* Absolute axes, mouse buttons and a wheel, and nothing touch-like, is
     * what udev and libinput classify as an absolute pointer -- the same
     * shape as a VM's USB tablet. */
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) || ioctl(fd, UI_SET_EVBIT, EV_ABS) ||
        ioctl(fd, UI_SET_EVBIT, EV_REL) ||
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL) || ioctl(fd, UI_SET_RELBIT, REL_HWHEEL) ||
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES) || ioctl(fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES))
        goto fail;
    for (int button = BTN_LEFT; button <= BTN_EXTRA; button++) {
        if (ioctl(fd, UI_SET_KEYBIT, button))
            goto fail;
    }
    if (ioctl(fd, UI_ABS_SETUP, &abs_x) || ioctl(fd, UI_ABS_SETUP, &abs_y) ||
        ioctl(fd, UI_DEV_SETUP, &setup) || ioctl(fd, UI_DEV_CREATE))
        goto fail;
    return fd;

fail:
    close(fd);
    return -1;
}

static int
create_keyboard(void)
{
    struct uinput_setup setup = {
        .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1d6b, .product = 0x0105 },
        .name = "Weaselway keyboard",
    };
    int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY))
        goto fail;
    /* Everything below the button range, which is where keyboards live. */
    for (int key = KEY_ESC; key < BTN_MISC; key++) {
        if (ioctl(fd, UI_SET_KEYBIT, key))
            goto fail;
    }
    if (ioctl(fd, UI_DEV_SETUP, &setup) || ioctl(fd, UI_DEV_CREATE))
        goto fail;
    return fd;

fail:
    close(fd);
    return -1;
}

struct input *
input_new(void)
{
    struct input *input = calloc(1, sizeof(*input));

    if (!input)
        return NULL;

    pthread_mutex_init(&input->lock, NULL);
    input->pointer_fd = create_pointer();
    input->keyboard_fd = input->pointer_fd >= 0 ? create_keyboard() : -1;
    if (input->pointer_fd < 0 || input->keyboard_fd < 0) {
        fprintf(stderr, "presenter: cannot create the uinput devices (%s) -- no input. "
                        "Is the uinput module loaded and /dev/uinput writable?\n",
                strerror(errno));
        input_free(input);
        return NULL;
    }

    fprintf(stderr, "presenter: uinput pointer and keyboard created\n");
    return input;
}

void
input_free(struct input *input)
{
    if (!input)
        return;

    if (input->pointer_fd >= 0) {
        ioctl(input->pointer_fd, UI_DEV_DESTROY);
        close(input->pointer_fd);
    }
    if (input->keyboard_fd >= 0) {
        ioctl(input->keyboard_fd, UI_DEV_DESTROY);
        close(input->keyboard_fd);
    }
    pthread_mutex_destroy(&input->lock);
    free(input);
}

void
input_pointer_motion(struct input *input, int x, int y, int width, int height)
{
    if (width < 2 || height < 2)
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > width - 1) x = width - 1;
    if (y > height - 1) y = height - 1;

    pthread_mutex_lock(&input->lock);
    emit(input->pointer_fd, EV_ABS, ABS_X, (int32_t)((int64_t)x * POINTER_ABS_MAX / (width - 1)));
    emit(input->pointer_fd, EV_ABS, ABS_Y, (int32_t)((int64_t)y * POINTER_ABS_MAX / (height - 1)));
    emit(input->pointer_fd, EV_SYN, SYN_REPORT, 0);
    pthread_mutex_unlock(&input->lock);
}

void
input_pointer_button(struct input *input, uint16_t button, bool pressed)
{
    if (button < BTN_LEFT || button >= BTN_LEFT + N_BUTTONS)
        return;

    pthread_mutex_lock(&input->lock);
    if (input->buttons[button - BTN_LEFT] != pressed) {
        input->buttons[button - BTN_LEFT] = pressed;
        emit(input->pointer_fd, EV_KEY, button, pressed);
        emit(input->pointer_fd, EV_SYN, SYN_REPORT, 0);
    }
    pthread_mutex_unlock(&input->lock);
}

void
input_pointer_wheel(struct input *input, int value120, bool horizontal)
{
    int *rest = &input->wheel_rest[horizontal];
    int notches;

    if (!value120)
        return;

    pthread_mutex_lock(&input->lock);
    emit(input->pointer_fd, EV_REL, horizontal ? REL_HWHEEL_HI_RES : REL_WHEEL_HI_RES, value120);

    *rest += value120;
    notches = *rest / 120;
    if (notches) {
        emit(input->pointer_fd, EV_REL, horizontal ? REL_HWHEEL : REL_WHEEL, notches);
        *rest -= notches * 120;
    }
    emit(input->pointer_fd, EV_SYN, SYN_REPORT, 0);
    pthread_mutex_unlock(&input->lock);
}

void
input_key(struct input *input, uint16_t key, bool pressed)
{
    if (key < KEY_ESC || key >= BTN_MISC)
        return;

    pthread_mutex_lock(&input->lock);
    /* A repeat arrives as another press; the compositor repeats by itself. */
    if (input->keys[key] != pressed) {
        input->keys[key] = pressed;
        emit(input->keyboard_fd, EV_KEY, key, pressed);
        emit(input->keyboard_fd, EV_SYN, SYN_REPORT, 0);
    }
    pthread_mutex_unlock(&input->lock);
}

void
input_release_all(struct input *input)
{
    pthread_mutex_lock(&input->lock);
    for (int i = 0; i < N_BUTTONS; i++) {
        if (input->buttons[i]) {
            input->buttons[i] = false;
            emit(input->pointer_fd, EV_KEY, (uint16_t)(BTN_LEFT + i), 0);
        }
    }
    emit(input->pointer_fd, EV_SYN, SYN_REPORT, 0);
    for (int key = 0; key <= KEY_MAX; key++) {
        if (input->keys[key]) {
            input->keys[key] = false;
            emit(input->keyboard_fd, EV_KEY, (uint16_t)key, 0);
        }
    }
    emit(input->keyboard_fd, EV_SYN, SYN_REPORT, 0);
    pthread_mutex_unlock(&input->lock);
}
