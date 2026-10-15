/* dolload.h - leave the browser: start another .dol from the SD card (such as
 * Swiss), or restart the console. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Checks a .dol file without running it. Fills err on failure. */
bool dol_check(const char *path, char *err, size_t errlen);

/* Loads and starts the .dol. Only returns if something went wrong before the
 * point of no return (err says what). */
bool dol_launch(const char *path, char *err, size_t errlen);

/* Hot-resets the console (returns to Swiss when it boots at power-on). */
void system_restart(void);

/* Finds .dol files on the card (root and two folder levels), sorted.
 * Returns the number found; paths go into out[i] (each 256 bytes). */
int dol_find(const char *root, char (*out)[256], int max);
