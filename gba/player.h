/* player.h - the music player's screens and buttons (no hardware here, so
 * it can also be tried out on a PC). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* button bits, as the GBA's KEYINPUT register (but 1 = pressed) */
#define K_A      0x001
#define K_B      0x002
#define K_SELECT 0x004
#define K_START  0x008
#define K_RIGHT  0x010
#define K_LEFT   0x020
#define K_UP     0x040
#define K_DOWN   0x080
#define K_R      0x100
#define K_L      0x200

enum { LINK_WAIT, LINK_OK, LINK_LOST };

#ifndef EWRAM_BSS
#ifdef __arm__
#define EWRAM_BSS __attribute__((section(".sbss")))   /* the GBA's larger, slower RAM */
#else
#define EWRAM_BSS
#endif
#endif

void player_init(void);
void player_message(int type, const uint8_t *data, int len);   /* a message from the GameCube */
void player_link(int state);                                   /* LINK_* each frame */
void player_input(unsigned pressed, unsigned repeat);          /* new presses / auto-repeats */
void player_frame(void);                                       /* once per frame (animations) */
bool player_needs_draw(void);
int  player_missing_song(void);                                /* first song not received (-1 none, or still coming) */
void player_draw(void);                                        /* into fb (gfx.h) */

/* provided by the program around it */
void player_send(int cmd, int arg);
void player_screen_power(bool on);
