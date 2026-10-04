/* The clipboard's conversions; run by `make check`. */

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "formats.h"

#define assert_bytes_equal_string(bytes, string) \
    g_assert_cmpmem(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), string, strlen(string))

static void
test_text_to_unicode(void)
{
    /* "a\nb" and U+1F600, which takes a surrogate pair. */
    static const char text[] = "a\nb\xf0\x9f\x98\x80";
    static const guint8 expected[] = { 'a', 0, '\r', 0, '\n', 0, 'b', 0,
                                       0x3d, 0xd8, 0x00, 0xde, 0, 0 };
    g_autoptr(GBytes) in = g_bytes_new_static(text, strlen(text));
    g_autoptr(GBytes) unicode = formats_text_to_unicode(in);
    g_autoptr(GBytes) back = NULL;

    g_assert_nonnull(unicode);
    g_assert_cmpmem(g_bytes_get_data(unicode, NULL), g_bytes_get_size(unicode), expected,
                    sizeof(expected));

    back = formats_text_from_unicode(unicode);
    g_assert_nonnull(back);
    assert_bytes_equal_string(back, text);
}

static void
test_text_edge_cases(void)
{
    static const char crlf[] = "a\r\nb";
    static const char invalid[] = "a\xff" "b\0ignored";
    static const guint8 odd[] = { 'x', 0, 'y' };
    g_autoptr(GBytes) empty = g_bytes_new_static("", 0);
    g_autoptr(GBytes) unicode = formats_text_to_unicode(empty);
    g_autoptr(GBytes) text = NULL;
    g_autoptr(GBytes) in = NULL;

    /* Nothing is still a terminated string. */
    g_assert_cmpuint(g_bytes_get_size(unicode), ==, 2);
    text = formats_text_from_unicode(empty);
    g_assert_cmpuint(g_bytes_get_size(text), ==, 0);
    g_clear_pointer(&unicode, g_bytes_unref);
    g_clear_pointer(&text, g_bytes_unref);

    /* A line that already ends in CR LF is left alone. */
    in = g_bytes_new_static(crlf, strlen(crlf));
    unicode = formats_text_to_unicode(in);
    g_assert_cmpuint(g_bytes_get_size(unicode), ==, (4 + 1) * 2);
    text = formats_text_from_unicode(unicode);
    assert_bytes_equal_string(text, "a\nb");
    g_clear_pointer(&unicode, g_bytes_unref);
    g_clear_pointer(&text, g_bytes_unref);
    g_clear_pointer(&in, g_bytes_unref);

    /* Invalid UTF-8 is replaced, and a NUL ends the text. */
    in = g_bytes_new_static(invalid, sizeof(invalid) - 1);
    unicode = formats_text_to_unicode(in);
    text = formats_text_from_unicode(unicode);
    assert_bytes_equal_string(text, "a\xef\xbf\xbd" "b");
    g_clear_pointer(&text, g_bytes_unref);
    g_clear_pointer(&in, g_bytes_unref);

    /* Without a terminator, and with half a unit at the end. */
    in = g_bytes_new_static(odd, sizeof(odd));
    text = formats_text_from_unicode(in);
    assert_bytes_equal_string(text, "x");
}

static void
test_html(void)
{
    static const char fragment[] = "<b>bold</b> \xc3\xa4";
    g_autoptr(GBytes) in = g_bytes_new_static(fragment, strlen(fragment));
    g_autoptr(GBytes) cf_html = formats_html_to_cf_html(in);
    g_autoptr(GBytes) back = NULL;
    const char *text;
    gsize size;
    guint start, end, start_html, end_html;

    g_assert_nonnull(cf_html);
    text = g_bytes_get_data(cf_html, &size);
    g_assert_cmpint(text[size - 1], ==, '\0');
    g_assert_true(g_str_has_prefix(text, "Version:0.9\r\nStartHTML:"));

    /* The offsets have to point at what they say. */
    g_assert_cmpint(sscanf(text,
                           "Version:0.9\r\nStartHTML:%u\r\nEndHTML:%u\r\nStartFragment:%u\r\n"
                           "EndFragment:%u\r\n",
                           &start_html, &end_html, &start, &end),
                    ==, 4);
    g_assert_cmpuint(end_html, ==, size - 1);
    g_assert_true(g_str_has_prefix(text + start_html, "<html><body>"));
    g_assert_cmpmem(text + start, end - start, fragment, strlen(fragment));
    g_assert_true(g_str_has_prefix(text + end, "<!--EndFragment-->"));

    back = formats_html_from_cf_html(cf_html);
    g_assert_nonnull(back);
    text = g_bytes_get_data(back, &size);
    g_assert_true(g_str_has_prefix(text, "<meta http-equiv=\"content-type\""));
    g_assert_true(size >= strlen(fragment));
    g_assert_cmpmem(text + size - strlen(fragment), strlen(fragment), fragment, strlen(fragment));
}

static void
test_html_from_windows(void)
{
    /* As a browser on Windows writes it: a context around the fragment. */
    static const char cf_html[] = "Version:0.9\r\n"
                                  "StartHTML:0000000105\r\n"
                                  "EndHTML:0000000199\r\n"
                                  "StartFragment:0000000141\r\n"
                                  "EndFragment:0000000163\r\n"
                                  "<html>\r\n<body>\r\n<!--StartFragment-->"
                                  "<i>from windows</i>   "
                                  "<!--EndFragment-->\r\n</body>\r\n</html>";
    static const char garbage[] = "no header here";
    g_autoptr(GBytes) in = g_bytes_new_static(cf_html, sizeof(cf_html));
    g_autoptr(GBytes) html = formats_html_from_cf_html(in);
    g_autoptr(GBytes) bad = g_bytes_new_static(garbage, strlen(garbage));
    const char *text;
    gsize size;

    g_assert_cmpuint(strstr(cf_html, "<i>") - cf_html, ==, 141);
    g_assert_nonnull(html);
    text = g_bytes_get_data(html, &size);
    g_assert_true(size > 22);
    g_assert_cmpmem(text + size - 22, 22, "<i>from windows</i>   ", 22);

    g_assert_null(formats_html_from_cf_html(bad));
}

static void
test_html_document(void)
{
    /* UTF-16 with a byte order mark, and a whole document. */
    static const char document[] = "<HTML><body>x</body></HTML>";
    g_autofree gunichar2 *utf16 = g_utf8_to_utf16(document, -1, NULL, NULL, NULL);
    g_autoptr(GByteArray) array = g_byte_array_new();
    g_autoptr(GBytes) in = NULL;
    g_autoptr(GBytes) cf_html = NULL;
    const char *text;
    guint start, end, start_html, end_html;

    g_byte_array_append(array, (const guint8 *)"\xff\xfe", 2);
    g_byte_array_append(array, (const guint8 *)utf16, strlen(document) * 2);
    in = g_byte_array_free_to_bytes(g_steal_pointer(&array));

    cf_html = formats_html_to_cf_html(in);
    g_assert_nonnull(cf_html);
    text = g_bytes_get_data(cf_html, NULL);
    g_assert_cmpint(sscanf(text,
                           "Version:0.9\r\nStartHTML:%u\r\nEndHTML:%u\r\nStartFragment:%u\r\n"
                           "EndFragment:%u\r\n",
                           &start_html, &end_html, &start, &end),
                    ==, 4);
    g_assert_cmpuint(start, ==, start_html);
    g_assert_cmpmem(text + start, end - start, document, strlen(document));
}

/* 3 x 2: red, green, blue over black, white, half-transparent red. */
static const guint8 test_pixels[] = {
    255, 0, 0, 255,   0, 255, 0, 255,       0, 0, 255, 255,
    0, 0, 0, 255,     255, 255, 255, 255,   255, 0, 0, 128,
};

static void
test_png_to_dib(void)
{
    /* Made with the same library, so this is the conversion and not the
     * codec that is tested. */
    g_autoptr(GBytes) png = NULL;
    g_autoptr(GBytes) dib = NULL;
    g_autoptr(GBytes) back = NULL;
    g_autoptr(GBytes) bmp = NULL;
    g_autoptr(GBytes) dib2 = NULL;
    const guint8 *data;
    gsize size;

    {
        extern GBytes *test_make_png(const guint8 *rgba, int width, int height);

        png = test_make_png(test_pixels, 3, 2);
    }
    g_assert_nonnull(png);

    dib = formats_png_to_dib(png);
    g_assert_nonnull(dib);
    data = g_bytes_get_data(dib, &size);
    /* 40 bytes of header, two rows of 3 * 3 bytes padded to 12. */
    g_assert_cmpuint(size, ==, 40 + 2 * 12);
    g_assert_cmpuint(data[0], ==, 40);
    g_assert_cmpuint(data[4], ==, 3);
    g_assert_cmpuint(data[8], ==, 2);
    g_assert_cmpuint(data[14], ==, 24);
    /* Bottom row first, B G R: black, white, and red over white. */
    g_assert_cmpmem(data + 40, 6, "\x00\x00\x00\xff\xff\xff", 6);
    g_assert_cmpuint(data[40 + 6 + 2], ==, 255);
    g_assert_cmpuint(data[40 + 6 + 1], >, 100);
    g_assert_cmpuint(data[40 + 6 + 1], <, 220);
    /* Then the top row: red, green, blue. */
    g_assert_cmpmem(data + 40 + 12, 9, "\x00\x00\xff\x00\xff\x00\xff\x00\x00", 9);

    /* And back, through both ways out of a bitmap. */
    back = formats_png_from_dib(dib);
    g_assert_nonnull(back);
    dib2 = formats_png_to_dib(back);
    g_assert_nonnull(dib2);
    g_assert_true(g_bytes_equal(dib, dib2));

    bmp = formats_bmp_from_dib(dib);
    g_assert_nonnull(bmp);
    data = g_bytes_get_data(bmp, &size);
    g_assert_cmpuint(size, ==, 14 + 40 + 24);
    g_assert_cmpmem(data, 2, "BM", 2);
    g_assert_cmpuint(data[2], ==, 14 + 40 + 24);
    g_assert_cmpuint(data[10], ==, 14 + 40);
    g_clear_pointer(&dib2, g_bytes_unref);
    dib2 = formats_bmp_to_dib(bmp);
    g_assert_true(g_bytes_equal(dib, dib2));
}

static void
test_dib_32_bit(void)
{
    /* 2 x 1, top-down, 32 bits with the masks after the header, in the
     * order R G B on the wire rather than the usual B G R. */
    static const guint8 dib[] = {
        40, 0, 0, 0,  2, 0, 0, 0,  0xff, 0xff, 0xff, 0xff,  1, 0,  32, 0,
        3, 0, 0, 0,   8, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        0xff, 0, 0, 0,  0, 0xff, 0, 0,  0, 0, 0xff, 0,
        10, 20, 30, 0,  40, 50, 60, 0,
    };
    g_autoptr(GBytes) in = g_bytes_new_static(dib, sizeof(dib));
    g_autoptr(GBytes) png = formats_png_from_dib(in);
    g_autoptr(GBytes) back = NULL;
    const guint8 *data;

    g_assert_nonnull(png);
    back = formats_png_to_dib(png);
    g_assert_nonnull(back);
    data = g_bytes_get_data(back, NULL);
    g_assert_cmpmem(data + 40, 6, "\x1e\x14\x0a\x3c\x32\x28", 6);
}

static void
test_dib_rejects(void)
{
    /* A header that promises more pixels than there are. */
    static const guint8 truncated[] = {
        40, 0, 0, 0,  100, 0, 0, 0,  100, 0, 0, 0,  1, 0,  24, 0,
        0, 0, 0, 0,   0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        1, 2, 3, 4,
    };
    g_autoptr(GBytes) in = g_bytes_new_static(truncated, sizeof(truncated));
    g_autoptr(GBytes) small = g_bytes_new_static(truncated, 10);
    g_autoptr(GBytes) empty = g_bytes_new_static("", 0);

    g_assert_null(formats_png_from_dib(in));
    g_assert_null(formats_bmp_from_dib(in));
    g_assert_null(formats_png_from_dib(small));
    g_assert_null(formats_png_from_dib(empty));
    g_assert_null(formats_png_to_dib(in));
    g_assert_null(formats_png_to_dib(empty));
    g_assert_null(formats_bmp_to_dib(small));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/formats/text/to-unicode", test_text_to_unicode);
    g_test_add_func("/formats/text/edge-cases", test_text_edge_cases);
    g_test_add_func("/formats/html/round-trip", test_html);
    g_test_add_func("/formats/html/from-windows", test_html_from_windows);
    g_test_add_func("/formats/html/document", test_html_document);
    g_test_add_func("/formats/image/png-to-dib", test_png_to_dib);
    g_test_add_func("/formats/image/dib-32-bit", test_dib_32_bit);
    g_test_add_func("/formats/image/rejects", test_dib_rejects);

    return g_test_run();
}

#include <png.h>

GBytes *
test_make_png(const guint8 *rgba, int width, int height)
{
    png_image image = { .version = PNG_IMAGE_VERSION, .width = (png_uint_32)width,
                        .height = (png_uint_32)height, .format = PNG_FORMAT_RGBA };
    gsize size = 0;
    guint8 *png;

    if (!png_image_write_get_memory_size(image, size, 0, rgba, 0, NULL))
        return NULL;
    png = g_malloc(size);
    if (!png_image_write_to_memory(&image, png, &size, 0, rgba, 0, NULL))
        return NULL;
    return g_bytes_new_take(png, size);
}
