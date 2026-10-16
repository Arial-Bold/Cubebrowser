/* storage.h - SD card (SD2SP2 / SD Gecko) mounting, the data folder, and file helpers.
 *
 * The data folder ("gcbrowser": settings, bookmarks, cookies, cacert.pem,
 * logs) is looked for in this order:
 *   1. next to cubebrowser.dol (when Swiss tells us where that is),
 *   2. one folder down from the root or from sd:/apps (e.g. sd:/apps/x/gcbrowser),
 *      when the .dol's location isn't known,
 *   3. sd:/gcbrowser at the root of the card (also the default for new cards).
 * If the chosen folder has no settings yet but sd:/gcbrowser does (data
 * from an older version), those files are moved over. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define STORAGE_ROOT_DIR "sd:/gcbrowser"

bool storage_init(const char *argv0);   /* argv[0] from main, may be NULL */
bool storage_ok(void);
const char *storage_device_name(void);

const char *storage_dir(void);               /* the data folder in use, e.g. "sd:/apps/cubebrowser/gcbrowser" */
void storage_path(char *out, size_t cap, const char *name);   /* storage_dir() + "/" + name */
const char *storage_program_dir(void);       /* folder holding the .dol, or NULL if unknown */
bool storage_dir_is_root(void);              /* data folder is sd:/gcbrowser */
const char *storage_moved_from(void);        /* old folder if data was moved at start-up, else NULL */

/* Moves the data folder to the root of the card or next to the .dol.
 * Files already in the destination are kept. msg gets a short result. */
bool storage_move_data(bool to_root, char *msg, size_t cap);

char *storage_read(const char *path, size_t *len_out);  /* malloc'd, NUL-terminated */
bool storage_write_atomic(const char *path, const char *tmp_path, const void *data, size_t len);
