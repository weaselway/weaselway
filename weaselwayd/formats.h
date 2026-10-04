/*
 * What is on a clipboard, between the forms it has on the two sides: Windows
 * clipboard formats on the RDP wire, mime types in the session.
 *
 * Every function returns NULL for data it cannot make sense of.
 */

#ifndef WEASELWAY_FORMATS_H
#define WEASELWAY_FORMATS_H

#include <glib.h>

/* CF_UNICODETEXT: UTF-16LE, lines end in CR LF, terminated by a NUL. The
 * session's text is UTF-8 with LF. */
GBytes *formats_text_from_unicode(GBytes *unicode);
GBytes *formats_text_to_unicode(GBytes *text);

/* "HTML Format": UTF-8 HTML behind a header of byte offsets that say where
 * the copied fragment is. The session's text/html is that fragment. */
GBytes *formats_html_from_cf_html(GBytes *cf_html);
GBytes *formats_html_to_cf_html(GBytes *html);

/* CF_DIB: a BITMAPINFOHEADER and the pixels, which is a .bmp file without its
 * first 14 bytes. */
GBytes *formats_png_from_dib(GBytes *dib);
GBytes *formats_png_to_dib(GBytes *png);
GBytes *formats_bmp_from_dib(GBytes *dib);
GBytes *formats_bmp_to_dib(GBytes *bmp);

#endif
