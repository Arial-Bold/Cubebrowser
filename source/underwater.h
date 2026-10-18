/* underwater.h - the loading screen: an underwater scene (rippling light on
 * the surface above, swaying rays, rising bubbles) drawn by the GPU while a
 * page loads, so the page appears all at once instead of piece by piece.
 * Everything is made in code; the only memory it keeps is a 4 KB pattern. */
#pragma once
#include <stdbool.h>

/* Draws the scene into the rounded page area. `alpha` fades the whole scene
 * (0..1), `t` is the time in seconds. */
void underwater_draw(float x, float y, float w, float h, float alpha, float t);
