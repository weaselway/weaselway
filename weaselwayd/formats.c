#define G_LOG_DOMAIN "clipboard"

#include "formats.h"

#include <stdbool.h>
#include <string.h>

#include <png.h>

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

GBytes *
formats_text_from_unicode(GBytes *unicode)
{
    gsize size, units, length = 0;
    const guint8 *data = g_bytes_get_data(unicode, &size);
    g_autofree gunichar2 *utf16 = NULL;
    g_autofree char *utf8 = NULL;
    GString *text;

    /* Copied, because nothing says the bytes are aligned. */
    units = size / sizeof(gunichar2);
    utf16 = g_new(gunichar2, units + 1);
    if (units)
        memcpy(utf16, data, units * sizeof(gunichar2));
    while (length < units && utf16[length]) {
        utf16[length] = GUINT16_FROM_LE(utf16[length]);
        length++;
    }

    utf8 = g_utf16_to_utf8(utf16, (glong)length, NULL, NULL, NULL);
    if (!utf8)
        return NULL;

    text = g_string_sized_new(strlen(utf8));
    for (const char *p = utf8; *p; p++) {
        if (p[0] == '\r' && p[1] == '\n')
            continue;
        g_string_append_c(text, *p);
    }
    return g_string_free_to_bytes(text);
}

GBytes *
formats_text_to_unicode(GBytes *text)
{
    gsize size;
    const char *data = g_bytes_get_data(text, &size);
    /* Some applications count a terminator in, and not every one of them
     * produces valid UTF-8. */
    g_autofree char *valid = g_utf8_make_valid(size ? data : "", size ? (gssize)strnlen(data, size) : 0);
    g_autoptr(GString) crlf = g_string_sized_new(strlen(valid));
    gunichar2 *utf16;
    glong units = 0;

    for (const char *p = valid; *p; p++) {
        if (*p == '\n' && (p == valid || p[-1] != '\r'))
            g_string_append_c(crlf, '\r');
        g_string_append_c(crlf, *p);
    }

    utf16 = g_utf8_to_utf16(crlf->str, (glong)crlf->len, NULL, &units, NULL);
    if (!utf16)
        return NULL;
    for (glong i = 0; i < units; i++)
        utf16[i] = GUINT16_TO_LE(utf16[i]);

    /* With the terminator, which g_utf8_to_utf16() has put there. */
    return g_bytes_new_take(utf16, (gsize)(units + 1) * sizeof(gunichar2));
}

/* ------------------------------------------------------------------ */
/* HTML                                                                */
/* ------------------------------------------------------------------ */

#define HTML_CHARSET "<meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\">"
#define FRAGMENT_START "<!--StartFragment-->"
#define FRAGMENT_END "<!--EndFragment-->"

/* The number after @key in the header, which is everything before the first
 * '<'. */
static bool
cf_html_offset(const char *text, const char *key, gsize *offset)
{
    const char *markup = strchr(text, '<');
    const char *found = strstr(text, key);
    char *end;

    if (!found || (markup && found > markup))
        return false;

    found += strlen(key);
    *offset = (gsize)g_ascii_strtoull(found, &end, 10);
    return end != found;
}

GBytes *
formats_html_from_cf_html(GBytes *cf_html)
{
    gsize size, length, start, end;
    const char *data = g_bytes_get_data(cf_html, &size);
    g_autofree char *text = g_strndup(data, size);
    const char *first, *last;
    GString *html;

    length = strlen(text);

    if (cf_html_offset(text, "StartFragment:", &start) &&
        cf_html_offset(text, "EndFragment:", &end) && start <= end && end <= length) {
        first = text + start;
        last = text + end;
    } else if ((first = strstr(text, FRAGMENT_START)) && (last = strstr(first, FRAGMENT_END))) {
        first += strlen(FRAGMENT_START);
    } else if (cf_html_offset(text, "StartHTML:", &start) &&
               cf_html_offset(text, "EndHTML:", &end) && start <= end && end <= length) {
        first = text + start;
        last = text + end;
    } else {
        return NULL;
    }

    /* Nothing in the session says which encoding text/html is in, and
     * browsers take this line for it. */
    html = g_string_new(HTML_CHARSET);
    g_string_append_len(html, first, last - first);
    return g_string_free_to_bytes(html);
}

static bool
contains_ignoring_case(const char *text, const char *lowercase)
{
    g_autofree char *lower = g_ascii_strdown(text, -1);

    return strstr(lower, lowercase) != NULL;
}

GBytes *
formats_html_to_cf_html(GBytes *html)
{
    static const char header_format[] = "Version:0.9\r\n"
                                        "StartHTML:%010u\r\n"
                                        "EndHTML:%010u\r\n"
                                        "StartFragment:%010u\r\n"
                                        "EndFragment:%010u\r\n";
    gsize size;
    const guint8 *data = g_bytes_get_data(html, &size);
    g_autofree char *text = NULL;
    g_autofree char *sized = g_strdup_printf(header_format, 0u, 0u, 0u, 0u);
    const char *before = "", *after = "";
    guint start_html, end_html, start_fragment, end_fragment;
    GString *cf_html;

    /* Firefox for one hands out UTF-16, and says so with a byte order mark. */
    if (size >= 2 && data[0] == 0xff && data[1] == 0xfe) {
        gsize units = (size - 2) / sizeof(gunichar2);
        g_autofree gunichar2 *utf16 = g_new(gunichar2, units + 1);

        memcpy(utf16, data + 2, units * sizeof(gunichar2));
        for (gsize i = 0; i < units; i++)
            utf16[i] = GUINT16_FROM_LE(utf16[i]);
        utf16[units] = 0;
        text = g_utf16_to_utf8(utf16, -1, NULL, NULL, NULL);
        if (!text)
            return NULL;
    } else {
        text = g_utf8_make_valid(size ? (const char *)data : "",
                                 size ? (gssize)strnlen((const char *)data, size) : 0);
    }

    /* A whole document is its own context; a fragment is given one, with the
     * comments Windows applications look for around it. */
    if (!contains_ignoring_case(text, "<html")) {
        before = "<html><body>\r\n" FRAGMENT_START;
        after = FRAGMENT_END "\r\n</body></html>";
    }

    start_html = (guint)strlen(sized);
    start_fragment = start_html + (guint)strlen(before);
    end_fragment = start_fragment + (guint)strlen(text);
    end_html = end_fragment + (guint)strlen(after);

    cf_html = g_string_new(NULL);
    g_string_printf(cf_html, header_format, start_html, end_html, start_fragment, end_fragment);
    g_string_append(cf_html, before);
    g_string_append(cf_html, text);
    g_string_append(cf_html, after);
    /* The format is a C string; the offsets do not count its terminator. */
    g_string_append_c(cf_html, '\0');
    return g_string_free_to_bytes(cf_html);
}

/* ------------------------------------------------------------------ */
/* Images                                                              */
/* ------------------------------------------------------------------ */

#define BMP_FILE_HEADER_SIZE 14
#define DIB_HEADER_SIZE 40 /* BITMAPINFOHEADER */
#define BI_RGB 0
#define BI_BITFIELDS 3

/* More than any screen, and little enough that sizes fit 32 bits. */
#define MAX_IMAGE_SIZE 16384

struct dib {
    int width, height;
    bool top_down;
    int bits;
    guint32 compression;
    /* Where the pixels start, and how far apart the rows are. */
    gsize offset, stride;
    /* Where red, green and blue are in a 32 bit pixel. */
    guint32 masks[3];
};

static guint16
le16(const guint8 *p)
{
    return (guint16)(p[0] | p[1] << 8);
}

static guint32
le32(const guint8 *p)
{
    return (guint32)p[0] | (guint32)p[1] << 8 | (guint32)p[2] << 16 | (guint32)p[3] << 24;
}

static void
put_le16(guint8 *p, guint16 value)
{
    p[0] = (guint8)value;
    p[1] = (guint8)(value >> 8);
}

static void
put_le32(guint8 *p, guint32 value)
{
    p[0] = (guint8)value;
    p[1] = (guint8)(value >> 8);
    p[2] = (guint8)(value >> 16);
    p[3] = (guint8)(value >> 24);
}

/* What the header says, if all of the image it describes is there. */
static bool
dib_parse(const guint8 *data, gsize size, struct dib *dib)
{
    guint32 header_size, colors;
    gint32 width, height;

    if (size < DIB_HEADER_SIZE)
        return false;

    header_size = le32(data);
    width = (gint32)le32(data + 4);
    height = (gint32)le32(data + 8);
    if (header_size < DIB_HEADER_SIZE || header_size > size || le16(data + 12) != 1 ||
        width <= 0 || width > MAX_IMAGE_SIZE || height == 0 || height == G_MININT32 ||
        ABS(height) > MAX_IMAGE_SIZE)
        return false;

    dib->width = width;
    dib->height = ABS(height);
    dib->top_down = height < 0;
    dib->bits = le16(data + 14);
    dib->compression = le32(data + 16);
    if (dib->bits != 1 && dib->bits != 4 && dib->bits != 8 && dib->bits != 16 &&
        dib->bits != 24 && dib->bits != 32)
        return false;

    dib->masks[0] = 0x00ff0000;
    dib->masks[1] = 0x0000ff00;
    dib->masks[2] = 0x000000ff;
    dib->offset = header_size;
    if (dib->compression == BI_BITFIELDS) {
        /* After a BITMAPINFOHEADER, inside the larger headers. */
        if (header_size == DIB_HEADER_SIZE)
            dib->offset += 12;
        if (size < DIB_HEADER_SIZE + 12)
            return false;
        for (int i = 0; i < 3; i++)
            dib->masks[i] = le32(data + DIB_HEADER_SIZE + 4 * i);
    }

    /* The color table: all of it unless the header says how much. */
    colors = le32(data + 32);
    if (!colors && dib->bits <= 8)
        colors = 1u << dib->bits;
    if (colors > 256)
        return false;
    dib->offset += colors * 4;

    dib->stride = (((gsize)dib->width * (gsize)dib->bits + 31) / 32) * 4;
    return dib->offset <= size && dib->stride * (gsize)dib->height <= size - dib->offset;
}

/* One channel of a pixel as eight bits. */
static guint8
channel(guint32 pixel, guint32 mask)
{
    int shift;

    if (!mask)
        return 0;
    shift = g_bit_nth_lsf(mask, -1);
    return (guint8)((guint64)((pixel & mask) >> shift) * 255 / (mask >> shift));
}

GBytes *
formats_png_from_dib(GBytes *dib_bytes)
{
    gsize size;
    const guint8 *data = g_bytes_get_data(dib_bytes, &size);
    g_autofree guint8 *rgb = NULL;
    g_autofree guint8 *png = NULL;
    png_image image = { .version = PNG_IMAGE_VERSION };
    png_alloc_size_t png_size;
    struct dib dib;
    bool plain;

    if (!data || !dib_parse(data, size, &dib))
        return NULL;
    /* What screenshots and image editors put on the clipboard. Whether the
     * fourth byte of a 32 bit pixel is an alpha is anybody's guess, so it is
     * not taken for one. */
    if (!(dib.bits == 24 && dib.compression == BI_RGB) &&
        !(dib.bits == 32 && (dib.compression == BI_RGB || dib.compression == BI_BITFIELDS))) {
        g_debug("a %d bit bitmap (compression %u) is not converted", dib.bits, dib.compression);
        return NULL;
    }

    /* B G R in memory, with or without a fourth byte: nearly every bitmap,
     * and no arithmetic per pixel. */
    plain = dib.bits == 24 || (dib.masks[0] == 0x00ff0000 && dib.masks[1] == 0x0000ff00 &&
                               dib.masks[2] == 0x000000ff);

    rgb = g_malloc((gsize)dib.width * 3 * (gsize)dib.height);
    for (int y = 0; y < dib.height; y++) {
        int row = dib.top_down ? y : dib.height - 1 - y;
        const guint8 *src = data + dib.offset + (gsize)row * dib.stride;
        guint8 *dst = rgb + (gsize)y * (gsize)dib.width * 3;
        int step = dib.bits / 8;

        for (int x = 0; x < dib.width; x++, dst += 3) {
            if (plain) {
                dst[0] = src[x * step + 2];
                dst[1] = src[x * step + 1];
                dst[2] = src[x * step];
            } else {
                guint32 pixel = le32(src + x * 4);

                dst[0] = channel(pixel, dib.masks[0]);
                dst[1] = channel(pixel, dib.masks[1]);
                dst[2] = channel(pixel, dib.masks[2]);
            }
        }
    }

    image.width = (png_uint_32)dib.width;
    image.height = (png_uint_32)dib.height;
    image.format = PNG_FORMAT_RGB;
    /* This runs on the main loop, between two frames, and the client is
     * waiting for it: a large file soon rather than a small one late. Hence
     * also the buffer for the worst case, where asking libpng for the size
     * would have it compress everything once more. */
    image.flags |= PNG_IMAGE_FLAG_FAST;
    png_size = PNG_IMAGE_PNG_SIZE_MAX(image);
    png = g_try_malloc(png_size);
    if (!png || !png_image_write_to_memory(&image, png, &png_size, 0, rgb, 0, NULL))
        return NULL;
    return g_bytes_new_take(g_realloc(g_steal_pointer(&png), png_size), png_size);
}

GBytes *
formats_png_to_dib(GBytes *png)
{
    gsize size, stride, pixels_size;
    const guint8 *data = g_bytes_get_data(png, &size);
    png_image image = { .version = PNG_IMAGE_VERSION };
    /* A bitmap has no transparency that applications agree on. */
    png_color white = { 255, 255, 255 };
    g_autofree guint8 *dib = NULL;

    if (!data || !png_image_begin_read_from_memory(&image, data, size))
        return NULL;
    if (image.width > MAX_IMAGE_SIZE || image.height > MAX_IMAGE_SIZE) {
        png_image_free(&image);
        return NULL;
    }

    /* Three bytes a pixel, rows padded to four, the bottom row first. */
    image.format = PNG_FORMAT_BGR;
    stride = ((gsize)image.width * 3 + 3) & ~(gsize)3;
    pixels_size = stride * image.height;
    dib = g_malloc0(DIB_HEADER_SIZE + pixels_size);

    if (!png_image_finish_read(&image, &white, dib + DIB_HEADER_SIZE, -(png_int_32)stride,
                               NULL)) {
        png_image_free(&image);
        return NULL;
    }

    put_le32(dib, DIB_HEADER_SIZE);
    put_le32(dib + 4, image.width);
    put_le32(dib + 8, image.height);
    put_le16(dib + 12, 1);
    put_le16(dib + 14, 24);
    put_le32(dib + 16, BI_RGB);
    put_le32(dib + 20, (guint32)pixels_size);
    /* 96 dots an inch, in pixels a metre. */
    put_le32(dib + 24, 3780);
    put_le32(dib + 28, 3780);
    return g_bytes_new_take(g_steal_pointer(&dib), DIB_HEADER_SIZE + pixels_size);
}

GBytes *
formats_bmp_from_dib(GBytes *dib_bytes)
{
    gsize size;
    const guint8 *data = g_bytes_get_data(dib_bytes, &size);
    struct dib dib;
    guint8 *bmp;

    if (!data || !dib_parse(data, size, &dib) || size > G_MAXUINT32 - BMP_FILE_HEADER_SIZE)
        return NULL;

    bmp = g_malloc(BMP_FILE_HEADER_SIZE + size);
    bmp[0] = 'B';
    bmp[1] = 'M';
    put_le32(bmp + 2, (guint32)(BMP_FILE_HEADER_SIZE + size));
    put_le32(bmp + 6, 0);
    put_le32(bmp + 10, (guint32)(BMP_FILE_HEADER_SIZE + dib.offset));
    memcpy(bmp + BMP_FILE_HEADER_SIZE, data, size);
    return g_bytes_new_take(bmp, BMP_FILE_HEADER_SIZE + size);
}

GBytes *
formats_bmp_to_dib(GBytes *bmp)
{
    gsize size;
    const guint8 *data = g_bytes_get_data(bmp, &size);
    struct dib dib;

    if (size < BMP_FILE_HEADER_SIZE || data[0] != 'B' || data[1] != 'M' ||
        !dib_parse(data + BMP_FILE_HEADER_SIZE, size - BMP_FILE_HEADER_SIZE, &dib))
        return NULL;

    return g_bytes_new_from_bytes(bmp, BMP_FILE_HEADER_SIZE, size - BMP_FILE_HEADER_SIZE);
}
