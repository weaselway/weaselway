#define G_LOG_DOMAIN "input"

#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <glib.h>
#include <linux/uinput.h>

#define POINTER_ABS_MAX 65535

#define N_BUTTONS (BTN_TASK - BTN_LEFT + 1)

/* The touchpad's surface: 150 x 100 mm, which is about what a large laptop pad
 * has, at 40 units a millimetre. libinput's gesture thresholds are in
 * millimetres, so the size matters; the real pad's is not known here. */
#define TOUCHPAD_RESOLUTION 40
#define TOUCHPAD_MAX_X (150 * TOUCHPAD_RESOLUTION)
#define TOUCHPAD_MAX_Y (100 * TOUCHPAD_RESOLUTION)
#define TOUCHPAD_SLOTS 5

struct input {
    int pointer_fd;
    int keyboard_fd;
    int touchpad_fd; /* -1 if it could not be created */

    /* The RDP callbacks write from different threads, the touch channel
     * having one of its own; an event and its SYN_REPORT have to stay
     * together. */
    GMutex lock;

    /* What is held down, so that a repeated press is not sent twice and a
     * client that disappears does not leave a key stuck. */
    bool buttons[N_BUTTONS];
    bool keys[KEY_MAX + 1];

    /* Wheel travel that did not add up to a whole notch yet. */
    int wheel_rest[2];

    /* The fingers on the touchpad, by multitouch slot. */
    struct {
        bool used;
        uint32_t id;
        int x, y;
    } slots[TOUCHPAD_SLOTS];
    int next_tracking_id;
    /* The BTN_TOOL_* that says how many there are, 0 for none. */
    uint16_t touchpad_tool;
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

static int
create_touchpad(void)
{
    /* On USB and with an id of its own: libinput pairs a touchpad it takes
     * to be built in with the keyboard and ignores it while keys are being
     * pressed, which is for palms and has no business here. */
    struct uinput_setup setup = {
        .id = { .bustype = BUS_USB, .vendor = 0x1d6b, .product = 0x0106 },
        .name = "Weaselway touchpad",
    };
    struct uinput_abs_setup axes[] = {
        { .code = ABS_X, .absinfo = { .maximum = TOUCHPAD_MAX_X, .resolution = TOUCHPAD_RESOLUTION } },
        { .code = ABS_Y, .absinfo = { .maximum = TOUCHPAD_MAX_Y, .resolution = TOUCHPAD_RESOLUTION } },
        { .code = ABS_MT_SLOT, .absinfo = { .maximum = TOUCHPAD_SLOTS - 1 } },
        { .code = ABS_MT_TRACKING_ID, .absinfo = { .maximum = 65535 } },
        { .code = ABS_MT_POSITION_X,
          .absinfo = { .maximum = TOUCHPAD_MAX_X, .resolution = TOUCHPAD_RESOLUTION } },
        { .code = ABS_MT_POSITION_Y,
          .absinfo = { .maximum = TOUCHPAD_MAX_Y, .resolution = TOUCHPAD_RESOLUTION } },
    };
    static const int keys[] = { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP,
                                BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
    int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;

    /* A pointer device with multitouch slots and one button under the whole
     * surface is a clickpad, as every current laptop has one. */
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) || ioctl(fd, UI_SET_EVBIT, EV_ABS) ||
        ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER) ||
        ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD))
        goto fail;
    for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, keys[i]))
            goto fail;
    }
    for (size_t i = 0; i < G_N_ELEMENTS(axes); i++) {
        if (ioctl(fd, UI_ABS_SETUP, &axes[i]))
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
    struct input *input = g_new0(struct input, 1);

    g_mutex_init(&input->lock);
    input->touchpad_fd = -1;
    input->pointer_fd = create_pointer();
    input->keyboard_fd = input->pointer_fd >= 0 ? create_keyboard() : -1;
    if (input->pointer_fd < 0 || input->keyboard_fd < 0) {
        g_warning("cannot create the uinput devices (%s) -- no input. "
                  "Is the uinput module loaded and /dev/uinput writable?",
                  g_strerror(errno));
        input_free(input);
        return NULL;
    }

    input->touchpad_fd = create_touchpad();
    g_message("uinput pointer, keyboard%s created",
              input->touchpad_fd >= 0 ? " and touchpad" : " (but no touchpad)");
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
    if (input->touchpad_fd >= 0) {
        ioctl(input->touchpad_fd, UI_DEV_DESTROY);
        close(input->touchpad_fd);
    }
    g_mutex_clear(&input->lock);
    g_free(input);
}

void
input_pointer_motion(struct input *input, int x, int y, int width, int height)
{
    if (width < 2 || height < 2)
        return;
    x = CLAMP(x, 0, width - 1);
    y = CLAMP(y, 0, height - 1);

    g_mutex_lock(&input->lock);
    emit(input->pointer_fd, EV_ABS, ABS_X, (int32_t)((int64_t)x * POINTER_ABS_MAX / (width - 1)));
    emit(input->pointer_fd, EV_ABS, ABS_Y, (int32_t)((int64_t)y * POINTER_ABS_MAX / (height - 1)));
    emit(input->pointer_fd, EV_SYN, SYN_REPORT, 0);
    g_mutex_unlock(&input->lock);
}

void
input_pointer_button(struct input *input, uint16_t button, bool pressed)
{
    if (button < BTN_LEFT || button >= BTN_LEFT + N_BUTTONS)
        return;

    g_mutex_lock(&input->lock);
    if (input->buttons[button - BTN_LEFT] != pressed) {
        input->buttons[button - BTN_LEFT] = pressed;
        emit(input->pointer_fd, EV_KEY, button, pressed);
        emit(input->pointer_fd, EV_SYN, SYN_REPORT, 0);
    }
    g_mutex_unlock(&input->lock);
}

void
input_pointer_wheel(struct input *input, int value120, bool horizontal)
{
    int *rest = &input->wheel_rest[horizontal];
    int notches;

    if (!value120)
        return;

    g_mutex_lock(&input->lock);
    emit(input->pointer_fd, EV_REL, horizontal ? REL_HWHEEL_HI_RES : REL_WHEEL_HI_RES, value120);

    *rest += value120;
    notches = *rest / 120;
    if (notches) {
        emit(input->pointer_fd, EV_REL, horizontal ? REL_HWHEEL : REL_WHEEL, notches);
        *rest -= notches * 120;
    }
    emit(input->pointer_fd, EV_SYN, SYN_REPORT, 0);
    g_mutex_unlock(&input->lock);
}

void
input_key(struct input *input, uint16_t key, bool pressed)
{
    if (key < KEY_ESC || key >= BTN_MISC)
        return;

    g_mutex_lock(&input->lock);
    /* A repeat arrives as another press; the compositor repeats by itself. */
    if (input->keys[key] != pressed) {
        input->keys[key] = pressed;
        emit(input->keyboard_fd, EV_KEY, key, pressed);
        emit(input->keyboard_fd, EV_SYN, SYN_REPORT, 0);
    }
    g_mutex_unlock(&input->lock);
}

void
input_touchpad_contact(struct input *input, uint32_t id, bool down, double x, double y)
{
    int fd = input->touchpad_fd, slot = -1, unused = -1;

    if (fd < 0)
        return;

    g_mutex_lock(&input->lock);
    for (int i = 0; i < TOUCHPAD_SLOTS; i++) {
        if (input->slots[i].used && input->slots[i].id == id)
            slot = i;
        else if (!input->slots[i].used && unused < 0)
            unused = i;
    }

    if (!down) {
        if (slot >= 0) {
            input->slots[slot].used = false;
            emit(fd, EV_ABS, ABS_MT_SLOT, slot);
            emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
        }
    } else if (slot >= 0 || unused >= 0) {
        /* A finger more than there are slots is not told about. */
        x = CLAMP(x, 0.0, 1.0);
        y = CLAMP(y, 0.0, 1.0);

        emit(fd, EV_ABS, ABS_MT_SLOT, slot >= 0 ? slot : unused);
        if (slot < 0) {
            slot = unused;
            input->slots[slot].used = true;
            input->slots[slot].id = id;
            input->next_tracking_id = (input->next_tracking_id + 1) & 0x7fff;
            emit(fd, EV_ABS, ABS_MT_TRACKING_ID, input->next_tracking_id);
        }
        input->slots[slot].x = (int)(x * TOUCHPAD_MAX_X + 0.5);
        input->slots[slot].y = (int)(y * TOUCHPAD_MAX_Y + 0.5);
        emit(fd, EV_ABS, ABS_MT_POSITION_X, input->slots[slot].x);
        emit(fd, EV_ABS, ABS_MT_POSITION_Y, input->slots[slot].y);
    }
    g_mutex_unlock(&input->lock);
}

/* The single-touch half of the protocol and the end of the frame. With the
 * lock held. */
static void
touchpad_frame_locked(struct input *input)
{
    static const uint16_t tools[] = { 0, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
                                      BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
    int fd = input->touchpad_fd, count = 0, first = -1;
    uint16_t tool;

    for (int i = 0; i < TOUCHPAD_SLOTS; i++) {
        if (!input->slots[i].used)
            continue;
        if (first < 0)
            first = i;
        count++;
    }

    tool = tools[count];
    if (tool != input->touchpad_tool) {
        if (input->touchpad_tool)
            emit(fd, EV_KEY, input->touchpad_tool, 0);
        if (!tool || !input->touchpad_tool)
            emit(fd, EV_KEY, BTN_TOUCH, tool != 0);
        if (tool)
            emit(fd, EV_KEY, tool, 1);
        input->touchpad_tool = tool;
    }
    if (first >= 0) {
        emit(fd, EV_ABS, ABS_X, input->slots[first].x);
        emit(fd, EV_ABS, ABS_Y, input->slots[first].y);
    }
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

void
input_touchpad_frame(struct input *input)
{
    if (input->touchpad_fd < 0)
        return;

    g_mutex_lock(&input->lock);
    touchpad_frame_locked(input);
    g_mutex_unlock(&input->lock);
}

static void
touchpad_release_locked(struct input *input)
{
    bool any = false;

    if (input->touchpad_fd < 0)
        return;

    for (int i = 0; i < TOUCHPAD_SLOTS; i++) {
        if (!input->slots[i].used)
            continue;
        input->slots[i].used = false;
        emit(input->touchpad_fd, EV_ABS, ABS_MT_SLOT, i);
        emit(input->touchpad_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
        any = true;
    }
    if (any)
        touchpad_frame_locked(input);
}

void
input_touchpad_release(struct input *input)
{
    g_mutex_lock(&input->lock);
    touchpad_release_locked(input);
    g_mutex_unlock(&input->lock);
}

void
input_release_all(struct input *input)
{
    g_mutex_lock(&input->lock);
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
    touchpad_release_locked(input);
    g_mutex_unlock(&input->lock);
}
