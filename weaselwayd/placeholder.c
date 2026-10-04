#include "placeholder.h"

#include <string.h>

#include <glib.h>

#define MESSAGE "No compositor running"

#define GLYPH_W 5
/* Seven rows for a capital, two more for the tails of p and g. */
#define GLYPH_H 9
/* One blank column between letters. */
#define ADVANCE (GLYPH_W + 1)

#define BACKGROUND 0x00202020u
#define FOREGROUND 0x00d8d8d8u

/* Nine rows of five bits, the leftmost pixel in bit 4. Just the letters of
 * MESSAGE: there is no font library here to ask for the rest. */
struct glyph {
    char c;
    uint8_t rows[GLYPH_H];
};

static const struct glyph glyphs[] = {
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11, 0x00, 0x00 } },
    { 'c', { 0x00, 0x00, 0x0e, 0x11, 0x10, 0x11, 0x0e, 0x00, 0x00 } },
    { 'g', { 0x00, 0x00, 0x0f, 0x11, 0x11, 0x11, 0x0f, 0x01, 0x0e } },
    { 'i', { 0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e, 0x00, 0x00 } },
    { 'm', { 0x00, 0x00, 0x1a, 0x15, 0x15, 0x15, 0x15, 0x00, 0x00 } },
    { 'n', { 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11, 0x00, 0x00 } },
    { 'o', { 0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e, 0x00, 0x00 } },
    { 'p', { 0x00, 0x00, 0x1e, 0x11, 0x11, 0x11, 0x1e, 0x10, 0x10 } },
    { 'r', { 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10, 0x00, 0x00 } },
    { 's', { 0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e, 0x00, 0x00 } },
    { 't', { 0x08, 0x08, 0x1c, 0x08, 0x08, 0x09, 0x06, 0x00, 0x00 } },
    { 'u', { 0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d, 0x00, 0x00 } },
};

static const struct glyph *
find_glyph(char c)
{
    for (gsize i = 0; i < G_N_ELEMENTS(glyphs); i++) {
        if (glyphs[i].c == c)
            return &glyphs[i];
    }
    return NULL;
}

static void
fill(uint8_t *pixels, int width, int x, int y, int w, int h, uint32_t color)
{
    for (int row = y; row < y + h; row++) {
        uint32_t *px = (uint32_t *)(void *)(pixels + ((size_t)row * (size_t)width + (size_t)x) * 4);

        for (int i = 0; i < w; i++)
            px[i] = color;
    }
}

void
placeholder_draw(uint8_t *pixels, int width, int height)
{
    int length = (int)strlen(MESSAGE);
    int columns = length * ADVANCE - 1;
    /* The text is about half as wide as the screen, in whole pixels per dot so
     * that the edges stay sharp. */
    int scale = CLAMP(width / 2 / columns, 1, 16);
    int x0 = (width - columns * scale) / 2;
    int y0 = (height - GLYPH_H * scale) / 2;

    fill(pixels, width, 0, 0, width, height, BACKGROUND);
    if (x0 < 0 || y0 < 0)
        return;

    for (int i = 0; i < length; i++) {
        const struct glyph *glyph = find_glyph(MESSAGE[i]);

        if (!glyph)
            continue;
        for (int row = 0; row < GLYPH_H; row++) {
            for (int col = 0; col < GLYPH_W; col++) {
                if (glyph->rows[row] & (1 << (GLYPH_W - 1 - col)))
                    fill(pixels, width, x0 + (i * ADVANCE + col) * scale, y0 + row * scale,
                         scale, scale, FOREGROUND);
            }
        }
    }
}
