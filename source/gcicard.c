/* gcicard.c - see gcicard.h */
#include "gcicard.h"
#include "storage.h"
#include "thr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#ifdef GEKKO
#include <gccore.h>
#include <ogc/card.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define BLOCK 8192
#define HDR 64
#define MAX_FILES 127

static uint8_t *gci;          /* the whole file, 32-byte aligned for the card hardware */
static int gci_len;
static GciInfo info;

static thr_mutex lock;
static bool lock_ok;
static GciSlot slots[2];
static volatile bool busy;
static bool want_check;
static int want_write = -1;
static bool want_replace;
static GciWriteState wstate;
static float wprog;
static char wmsg[160];

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void ensure_lock(void) { if (!lock_ok) lock_ok = thr_mutex_init(&lock); }

/* ------------------------------------------------------------ the file */

static void text_field(char *out, const uint8_t *src, int n) {
    int k = 0;
    for (int i = 0; i < n && src[i]; i++) out[k++] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : ' ';
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static const char *region_of(char c) {
    switch (c) {
    case 'E': return "USA";
    case 'P': return "Europe";
    case 'J': return "Japan";
    case 'K': return "Korea";
    case 'D': return "Germany";
    case 'F': return "France";
    case 'S': return "Spain";
    case 'I': return "Italy";
    case 'U': return "Australia";
    default: return "";
    }
}

void gci_unload(void) {
    free(gci);
    gci = NULL;
    gci_len = 0;
    memset(&info, 0, sizeof(info));
}

bool gci_load(const char *path, char *err, int errlen) {
    gci_unload();
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "The file couldn't be opened."); return false; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < HDR + BLOCK || len > HDR + 2043L * BLOCK) {
        fclose(f);
        snprintf(err, errlen, "This isn't a GameCube save (.gci) file.");
        return false;
    }
    gci = memalign(32, (size_t)len);
    if (!gci) { fclose(f); snprintf(err, errlen, "Not enough memory for this save."); return false; }
    bool ok = fread(gci, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (!ok) { gci_unload(); snprintf(err, errlen, "The file couldn't be read."); return false; }
    gci_len = (int)len;

    /* check the directory entry makes sense before anything goes near a card */
    int blocks = (int)be16(gci + 0x38);
    bool good = blocks > 0 && (long)HDR + (long)blocks * BLOCK == len && gci[8] != 0 && gci[8] != 0xff;
    for (int i = 0; i < 4 && good; i++) good = gci[i] >= 32 && gci[i] < 127;
    uint32_t icon = be32(gci + 0x2c), comment = be32(gci + 0x3c);
    if (good && comment != 0xffffffffu && comment + 64 > (uint32_t)blocks * BLOCK) good = false;
    if (good && icon != 0xffffffffu && icon >= (uint32_t)blocks * BLOCK) good = false;
    if (!good) {
        gci_unload();
        snprintf(err, errlen, "This file isn't a GameCube save in the .gci format, or it's damaged.");
        return false;
    }
    memcpy(info.code, gci, 4);
    memcpy(info.maker, gci + 4, 2);
    text_field(info.name, gci + 8, 32);
    info.region = region_of(info.code[3]);
    info.blocks = blocks;
    if (comment != 0xffffffffu) {
        text_field(info.title, gci + HDR + comment, 32);
        text_field(info.detail, gci + HDR + comment + 32, 32);
    }
    return true;
}

const GciInfo *gci_info(void) { return gci ? &info : NULL; }

/* ------------------------------------------------------- the card itself */

#ifdef GEKKO
static uint8_t workarea[CARD_WORKAREA] __attribute__((aligned(32)));
static bool card_inited;

/* Mounts a slot; fills in its state. Returns true if it's a usable memory card. */
static bool mount(int chn, GciSlot *s) {
    if (!card_inited) { CARD_Init("CUBE", "00"); card_inited = true; }
    s32 mem = 0, sect = 0;
    int r = CARD_ProbeEx(chn, &mem, &sect);
    for (int k = 0; r == CARD_ERROR_BUSY && k < 6; k++) { usleep(100000); r = CARD_ProbeEx(chn, &mem, &sect); }
    if (r == CARD_ERROR_NOCARD) { s->state = GC_SLOT_EMPTY; return false; }
    if (r == CARD_ERROR_WRONGDEVICE) { s->state = GC_SLOT_OTHER; return false; }
    if (r < 0) { s->state = GC_SLOT_BROKEN; return false; }
    if (sect != BLOCK) { s->state = GC_SLOT_OTHER; return false; }
    r = CARD_Mount(chn, workarea, NULL);
    if (r < 0 && r != CARD_ERROR_ENCODING) {
        CARD_Unmount(chn);
        s->state = r == CARD_ERROR_NOCARD ? GC_SLOT_EMPTY : GC_SLOT_BROKEN;
        return false;
    }
    return true;
}

/* Blocks and directory entries in use, and whether our save is there. */
static void survey(int chn, GciSlot *s) {
    u16 total = 0;
    CARD_GetBlockCount(chn, &total);
    int used = 0, files = 0;
    bool found = false;
    card_dir d;
    memset(&d, 0, sizeof(d));
    int r = CARD_FindFirst(chn, &d, true);
    while (r == CARD_ERROR_READY) {
        files++;
        used += (int)((d.filelen + BLOCK - 1) / BLOCK);
        if (gci && !memcmp(d.gamecode, gci, 4) && !memcmp(d.company, gci + 4, 2) && !strncmp(d.filename, (const char *)gci + 8, 32))
            found = true;
        r = CARD_FindNext(&d);
    }
    s->blocks_total = total > 5 ? total - 5 : 0;   /* 5 blocks hold the card's own tables */
    s->blocks_free = s->blocks_total - used;
    if (s->blocks_free < 0) s->blocks_free = 0;
    s->files_free = MAX_FILES - files;
    s->has_save = found;
    s->state = GC_SLOT_READY;
}

static void check_slot(int chn, GciSlot *out) {
    GciSlot s;
    memset(&s, 0, sizeof(s));
    if (mount(chn, &s)) {
        survey(chn, &s);
        CARD_Unmount(chn);
    }
    *out = s;
}

static const char *card_error(int r) {
    switch (r) {
    case CARD_ERROR_NOCARD: return "The memory card was taken out.";
    case CARD_ERROR_INSSPACE: return "There isn't enough room on the memory card.";
    case CARD_ERROR_NOENT: return "The memory card can't hold any more saves (127 at most).";
    case CARD_ERROR_EXIST: return "A save with this name is already on the card.";
    case CARD_ERROR_NOPERM: return "The save on the card is protected and can't be replaced.";
    case CARD_ERROR_BROKEN: return "The memory card seems damaged.";
    default: return NULL;
    }
}

static void set_progress(float p) { thr_lock(&lock); wprog = p; thr_unlock(&lock); }

/* returns NULL when it worked, or what went wrong */
static const char *write_save(int chn, bool replace, char *buf, int buflen) {
    GciSlot s;
    memset(&s, 0, sizeof(s));
    if (!mount(chn, &s)) {
        if (s.state == GC_SLOT_EMPTY) return "There's no memory card in that slot.";
        return "That memory card couldn't be read.";
    }
    survey(chn, &s);
    const char *why = NULL;
    char code[5] = { 0 }, maker[3] = { 0 }, name[CARD_FILENAMELEN + 1] = { 0 };
    memcpy(code, gci, 4);
    memcpy(maker, gci + 4, 2);
    memcpy(name, gci + 8, CARD_FILENAMELEN);
    CARD_SetGamecode(code);   /* the new save belongs to its game, not to the browser */
    CARD_SetCompany(maker);
    int blocks = info.blocks, r;
    card_file f;

    if (s.has_save) {
        if (!replace) { why = "A save with this name is already on the card."; goto out; }
        /* room for the new one once the old one is gone? */
        memset(&f, 0, sizeof(f));
        int old_blocks = 0;
        if (CARD_Open(chn, name, &f) == CARD_ERROR_READY) { old_blocks = (f.len + BLOCK - 1) / BLOCK; CARD_Close(&f); }
        if (s.blocks_free + old_blocks < blocks) { why = "There isn't enough room on the memory card."; goto out; }
        r = CARD_Delete(chn, name);
        if (r == CARD_ERROR_BUSY) { usleep(100000); r = CARD_Delete(chn, name); }
        if (r < 0) { why = card_error(r); if (!why) { snprintf(buf, buflen, "The old save couldn't be removed (error %d).", r); why = buf; } goto out; }
    } else {
        if (s.blocks_free < blocks) { why = "There isn't enough room on the memory card."; goto out; }
        if (s.files_free <= 0) { why = "The memory card can't hold any more saves (127 at most)."; goto out; }
    }

    memset(&f, 0, sizeof(f));
    r = CARD_Create(chn, name, (u32)blocks * BLOCK, &f);
    if (r == CARD_ERROR_BUSY) { usleep(100000); r = CARD_Create(chn, name, (u32)blocks * BLOCK, &f); }
    if (r < 0) { why = card_error(r); if (!why) { snprintf(buf, buflen, "The save couldn't be made on the card (error %d).", r); why = buf; } goto out; }

    for (int b = 0; b < blocks; b++) {
        r = CARD_Write(&f, gci + HDR + b * BLOCK, BLOCK, (u32)b * BLOCK);
        for (int k = 0; r == CARD_ERROR_BUSY && k < 10; k++) { usleep(50000); r = CARD_Write(&f, gci + HDR + b * BLOCK, BLOCK, (u32)b * BLOCK); }
        if (r < 0) break;
        set_progress((b + 1) / (float)(blocks + 1));
    }
    if (r < 0) {
        CARD_Close(&f);
        CARD_Delete(chn, name);   /* don't leave half a save behind */
        why = card_error(r);
        if (!why) { snprintf(buf, buflen, "Writing to the memory card failed (error %d). Nothing was kept.", r); why = buf; }
        goto out;
    }

    /* the icon, banner and comment, so the GameCube's memory card screen shows the save properly */
    card_stat st;
    if (CARD_GetStatus(chn, f.filenum, &st) == CARD_ERROR_READY) {
        st.banner_fmt = gci[0x07];
        st.icon_addr = be32(gci + 0x2c);
        st.icon_fmt = (u16)be16(gci + 0x30);
        st.icon_speed = (u16)be16(gci + 0x32);
        st.comment_addr = be32(gci + 0x3c);
        r = CARD_SetStatus(chn, f.filenum, &st);
        if (r < 0) { snprintf(buf, buflen, "The save was copied, but its icon and title may not show (error %d).", r); why = buf; }
    }
    uint8_t perm = gci[0x34];
    if (perm != CARD_ATTRIB_PUBLIC) CARD_SetAttributes(chn, f.filenum, perm);
    CARD_Close(&f);
    set_progress(1);
out:
    CARD_SetGamecode("CUBE");
    CARD_SetCompany("00");
    CARD_Unmount(chn);
    return why;
}
#else
/* PC tests: the memory cards are folders of .gci files, "memcard_a" and "memcard_b";
 * a file "memcard_a/size" holds the card's block count. */
static const char *dir_of(int chn) { return chn ? "memcard_b" : "memcard_a"; }

static void check_slot(int chn, GciSlot *out) {
    GciSlot s;
    memset(&s, 0, sizeof(s));
    usleep(300000);   /* a real card takes a moment */
    DIR *d = opendir(dir_of(chn));
    if (!d) { s.state = GC_SLOT_EMPTY; *out = s; return; }
    s.blocks_total = 59;
    char p[300];
    snprintf(p, sizeof(p), "%s/size", dir_of(chn));
    FILE *sf = fopen(p, "r");
    if (sf) { if (fscanf(sf, "%d", &s.blocks_total) != 1) s.blocks_total = 59; fclose(sf); }
    int used = 0, files = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".gci")) continue;
        snprintf(p, sizeof(p), "%s/%s", dir_of(chn), e->d_name);
        struct stat st;
        if (stat(p, &st) == 0) used += (int)((st.st_size - HDR) / BLOCK);
        files++;
        if (gci) {
            char want[60];
            snprintf(want, sizeof(want), "%.4s-%.32s.gci", info.code, info.name);
            if (!strcmp(e->d_name, want)) s.has_save = true;
        }
    }
    closedir(d);
    s.blocks_free = s.blocks_total - used;
    s.files_free = MAX_FILES - files;
    s.state = GC_SLOT_READY;
    *out = s;
}

static void set_progress(float p) { thr_lock(&lock); wprog = p; thr_unlock(&lock); }

static const char *write_save(int chn, bool replace, char *buf, int buflen) {
    (void)buf; (void)buflen;
    GciSlot s;
    check_slot(chn, &s);
    if (s.state != GC_SLOT_READY) return "There's no memory card in that slot.";
    if (s.has_save && !replace) return "A save with this name is already on the card.";
    if (!s.has_save && s.blocks_free < info.blocks) return "There isn't enough room on the memory card.";
    for (int b = 0; b < info.blocks; b++) { usleep(40000); set_progress((b + 1) / (float)(info.blocks + 1)); }
    char p[300];
    snprintf(p, sizeof(p), "%s/%.4s-%.32s.gci", dir_of(chn), info.code, info.name);
    FILE *f = fopen(p, "wb");
    if (!f) return "Writing to the memory card failed.";
    fwrite(gci, 1, (size_t)gci_len, f);
    fclose(f);
    set_progress(1);
    return NULL;
}
#endif

/* ------------------------------------------------------------ the thread */

static void *job_main(void *arg) {
    (void)arg;
    for (;;) {
        thr_lock(&lock);
        bool chk = want_check;
        int w = want_write;
        bool rep = want_replace;
        want_check = false;
        want_write = -1;
        if (!chk && w < 0) { busy = false; thr_unlock(&lock); break; }
        thr_unlock(&lock);
        if (w >= 0) {
            static char buf[160];
            const char *why = gci ? write_save(w, rep, buf, sizeof(buf)) : "No save is loaded.";
            thr_lock(&lock);
            if (why) snprintf(wmsg, sizeof(wmsg), "%s", why);
            /* "copied, but..." still counts as done */
            wstate = (!why || !strncmp(why, "The save was copied", 19)) ? GCW_DONE : GCW_FAILED;
            thr_unlock(&lock);
            chk = true;   /* the card has changed */
        }
        if (chk) {
            for (int i = 0; i < 2; i++) {
                GciSlot s;
                if (!strcmp(storage_device_name(), i ? "SD Gecko (slot B)" : "SD Gecko (slot A)")) {
                    memset(&s, 0, sizeof(s));
                    s.state = GC_SLOT_SD;   /* never touch the SD card's slot as a memory card */
                } else {
                    check_slot(i, &s);
                }
                thr_lock(&lock);
                slots[i] = s;
                thr_unlock(&lock);
            }
        }
    }
    return NULL;
}

static void start_thread(void) {
    if (busy) return;   /* the running job picks the request up */
    busy = true;
    thr_t t;
    if (thr_create(&t, job_main, NULL, 32 * 1024, 50)) thr_detach(t);
    else busy = false;
}

void gci_check_slots(void) {
    ensure_lock();
    thr_lock(&lock);
    for (int i = 0; i < 2; i++)
        if (slots[i].state == GC_SLOT_UNKNOWN) slots[i].state = GC_SLOT_CHECKING;
    want_check = true;
    thr_unlock(&lock);
    start_thread();
}

const GciSlot *gci_slot(int i) {
    static GciSlot copy[2];
    ensure_lock();
    thr_lock(&lock);
    copy[i & 1] = slots[i & 1];
    thr_unlock(&lock);
    return &copy[i & 1];
}

void gci_write(int slot, bool replace) {
    ensure_lock();
    thr_lock(&lock);
    want_write = slot;
    want_replace = replace;
    wstate = GCW_WRITING;
    wprog = 0;
    wmsg[0] = 0;
    thr_unlock(&lock);
    start_thread();
}

GciWriteState gci_write_state(void) {
    ensure_lock();
    thr_lock(&lock);
    GciWriteState s = wstate;
    thr_unlock(&lock);
    return s;
}

float gci_write_progress(void) { return wprog; }
const char *gci_write_message(void) { return wmsg; }

void gci_write_reset(void) {
    ensure_lock();
    thr_lock(&lock);
    if (wstate != GCW_WRITING) { wstate = GCW_IDLE; wmsg[0] = 0; }
    thr_unlock(&lock);
}
