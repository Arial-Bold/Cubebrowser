/* gcicard.h - copies a downloaded GameCube save (.gci) onto a memory card.
 *
 * A .gci is the save exactly as the memory card stores it: a 64-byte
 * directory entry (the game's code, the save's name, where its icon and
 * comment are) followed by the save's 8 KB blocks. The whole file is read
 * into memory first, so the SD card isn't used while the memory card is
 * being written.
 *
 * The slow parts (looking at a card, writing) run on a background thread;
 * the screen asks how things stand once a frame. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GC_SLOT_UNKNOWN,     /* not looked at yet */
    GC_SLOT_CHECKING,
    GC_SLOT_READY,       /* a memory card: blocks_free is set */
    GC_SLOT_EMPTY,       /* nothing in the slot */
    GC_SLOT_SD,          /* the SD card adapter is in this slot */
    GC_SLOT_OTHER,       /* something that isn't a memory card */
    GC_SLOT_BROKEN,      /* a memory card that couldn't be read */
} GciSlotState;

typedef struct {
    GciSlotState state;
    int blocks_total;    /* blocks for saves (59, 251, 1019...) */
    int blocks_free;
    int files_free;      /* free directory entries (a card holds 127 saves) */
    bool has_save;       /* this save is already on the card */
} GciSlot;

typedef enum { GCW_IDLE, GCW_WRITING, GCW_DONE, GCW_FAILED } GciWriteState;

typedef struct {
    char code[5];        /* "GALE" */
    char maker[3];       /* "01" */
    char name[33];       /* the save's file name on the card */
    char title[33];      /* the comment's first line, usually the game's name */
    char detail[33];     /* its second line */
    const char *region;  /* "USA", "Europe", "Japan"... or "" */
    int blocks;
} GciInfo;

/* Reads a .gci; false (with a reason) if it isn't one. Replaces any loaded before. */
bool gci_load(const char *path, char *err, int errlen);
const GciInfo *gci_info(void);
void gci_unload(void);

/* Looks at both slots again (in the background). */
void gci_check_slots(void);
const GciSlot *gci_slot(int slot);   /* 0 = slot A, 1 = slot B */

/* Writes the loaded save to a slot; replace removes a save of the same name first. */
void gci_write(int slot, bool replace);
GciWriteState gci_write_state(void);
float gci_write_progress(void);      /* 0..1 */
const char *gci_write_message(void); /* why, when GCW_FAILED */
void gci_write_reset(void);
