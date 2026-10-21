/* gbalink.h - a Game Boy Advance on controller port 2 as the music player's
 * second screen and remote.
 *
 * When one is plugged in (switched on with no game inserted), the browser
 * sends it the player program over the link cable, then keeps it up to date
 * with what's playing, the song list and the instrument choices, and acts on
 * its buttons. It can be plugged in and out at any time. The on-screen music
 * player keeps working too. Off in Settings > Music frees port 2. */
#pragma once
#include <stdbool.h>

void gbalink_init(void);      /* after midi_init */
void gbalink_frame(void);     /* once per frame, on the main thread */
bool gbalink_linked(void);
