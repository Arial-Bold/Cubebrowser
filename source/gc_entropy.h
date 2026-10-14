/* entropy.h - randomness for TLS on hardware with no hardware RNG.
 *
 * Sources: a seed file saved on the SD card between runs, CPU timebase
 * jitter, controller input timing and network packet arrival timing, all
 * folded through SHA-256. This is reasonable for a hobby browser but is NOT
 * equivalent to a real hardware RNG; don't rely on it for high-value secrets.
 */
#pragma once
#include <gccore.h>
#include <stddef.h>

void entropy_init(void);                    /* loads the seed file */
void entropy_add(const void *data, size_t len);
void entropy_add_event(u32 value);          /* mixes value with the current timebase */
void entropy_save_seed(const unsigned char *seed32);
