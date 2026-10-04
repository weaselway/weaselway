/*
 * What the client sees while no compositor owns the display: a dark screen
 * with "No compositor running" in the middle.
 */

#ifndef WEASELWAY_PLACEHOLDER_H
#define WEASELWAY_PLACEHOLDER_H

#include <stdint.h>

/* Draws the screen into @pixels, @width * 4 bytes a row, B G R X. */
void placeholder_draw(uint8_t *pixels, int width, int height);

#endif
