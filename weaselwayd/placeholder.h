/*
 * What the client sees while no compositor owns the display: a dark screen
 * with "No compositor running" in the middle. While the compositor has the
 * display off: a black one with "Screen is off".
 */

#ifndef WEASELWAY_PLACEHOLDER_H
#define WEASELWAY_PLACEHOLDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Draws the screen into @pixels, @stride bytes a row, B G R X. */
void placeholder_draw(uint8_t *pixels, size_t stride, int width, int height, bool display_off);

#endif
