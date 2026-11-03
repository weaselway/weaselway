/*
 * What the client sees while no compositor owns the display: a dark screen
 * with "No compositor running" in the middle.
 */

#ifndef WEASELWAY_PLACEHOLDER_H
#define WEASELWAY_PLACEHOLDER_H

#include <stddef.h>
#include <stdint.h>

/* Draws the screen into @pixels, @stride bytes a row, B G R X. */
void placeholder_draw(uint8_t *pixels, size_t stride, int width, int height);

#endif
