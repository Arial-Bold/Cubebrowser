/* sound.h - small procedurally synthesised UI sounds (no sampled assets). */
#pragma once

typedef enum { SFX_HOVER, SFX_SELECT, SFX_BACK, SFX_ERROR, SFX_KEY, SFX_OPEN, SFX_CLOSE, SFX_LOADED, SFX_COUNT } Sfx;

void sound_init(void);
void sound_play(Sfx s);
