/* diag.h - freeze recorder. Each part of the program leaves a short note of
 * what it's doing ("breadcrumbs"). A watchdog thread notices when the screen
 * stops updating for 10 seconds and writes the notes to
 * sd:/gcbrowser/freeze.log, so a freeze can be traced afterwards. */
#pragma once

enum { DIAG_MAIN, DIAG_WORKER, DIAG_PAGE, DIAG_DRAW, DIAG_MUSIC, DIAG_SLOTS };

void diag_init(void);
void diag_note(int slot, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void diag_frame(void);   /* call once per frame */
const char *diag_get(int slot);
