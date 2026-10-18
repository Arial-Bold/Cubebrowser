/* dolload.c - starting another program (.dol) from the SD card.
 *
 * The new program normally loads into the same memory the browser runs from,
 * so it can't simply be copied into place. Instead:
 *   1. read the .dol and check it,
 *   2. shut down video, sound, graphics, the SD card and interrupts,
 *   3. move the file, a copy plan and a tiny self-contained copier routine
 *      into the top of memory, above everything the new program uses,
 *   4. switch to a small stack up there and jump to the copier, which copies
 *      each section into place, clears the BSS, flushes the caches and jumps
 *      to the program's entry point. */
#include "dolload.h"
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define DOL_SECTIONS 18          /* 7 text + 11 data */
#define MEM_LO 0x80000000u
#define MEM_HI 0x81800000u       /* end of the GameCube's 24 MB */
#define MAX_DOL (8u * 1024u * 1024u)

typedef struct {
    uint32_t off[DOL_SECTIONS], addr[DOL_SECTIONS], size[DOL_SECTIONS];
    uint32_t bss_addr, bss_size, entry;
    uint32_t image;              /* where the file's bytes are during the copy */
} DolPlan;

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Parses and sanity-checks a .dol header. max_end gets the highest address used. */
static bool parse(const unsigned char *d, size_t len, DolPlan *pl, uint32_t *max_end, char *err, size_t errlen) {
    if (len < 0x100) { snprintf(err, errlen, "That file is too small to be a program."); return false; }
    memset(pl, 0, sizeof(*pl));
    for (int i = 0; i < DOL_SECTIONS; i++) {
        pl->off[i] = be32(d + 4 * i);
        pl->addr[i] = be32(d + 0x48 + 4 * i);
        pl->size[i] = be32(d + 0x90 + 4 * i);
    }
    pl->bss_addr = be32(d + 0xD8);
    pl->bss_size = be32(d + 0xDC);
    pl->entry = be32(d + 0xE0);
    uint32_t hi = 0;
    bool entry_ok = false, any = false;
    for (int i = 0; i < DOL_SECTIONS; i++) {
        if (!pl->size[i]) continue;
        any = true;
        uint32_t a = pl->addr[i] | 0x80000000u, n = pl->size[i];
        if (pl->off[i] < 0x100 || (uint64_t)pl->off[i] + n > len || a < MEM_LO + 0x3000 || (uint64_t)a + n > MEM_HI) {
            snprintf(err, errlen, "That file isn't a valid GameCube program (section %d).", i);
            return false;
        }
        pl->addr[i] = a;
        if (a + n > hi) hi = a + n;
        if (i < 7 && pl->entry >= a && pl->entry < a + n) entry_ok = true;
    }
    if (!any || !entry_ok) { snprintf(err, errlen, "That file isn't a valid GameCube program."); return false; }
    if (pl->bss_size) {
        pl->bss_addr |= 0x80000000u;
        if ((uint64_t)pl->bss_addr + pl->bss_size > MEM_HI) { snprintf(err, errlen, "That program's memory layout is invalid."); return false; }
        if (pl->bss_addr + pl->bss_size > hi) hi = pl->bss_addr + pl->bss_size;
    }
    *max_end = hi;
    return true;
}

static unsigned char *read_file(const char *path, size_t *len, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "Couldn't find %s on the SD card. Check Settings > Data > Swiss .dol file.", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (unsigned long)n > MAX_DOL) { fclose(f); snprintf(err, errlen, "That file is too large to load."); return NULL; }
    unsigned char *b = malloc((size_t)n + 64);
    if (!b) { fclose(f); snprintf(err, errlen, "Not enough memory to load it."); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(b); snprintf(err, errlen, "Couldn't read the file."); return NULL; }
    *len = got;
    return b;
}

bool dol_check(const char *path, char *err, size_t errlen) {
    size_t len;
    unsigned char *b = read_file(path, &len, err, errlen);
    if (!b) return false;
    DolPlan pl;
    uint32_t hi;
    bool ok = parse(b, len, &pl, &hi, err, errlen);
    free(b);
    return ok;
}

/* ------------------------------------------------------------ finding */

static bool ends_dol(const char *n) {
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".dol");
}

static void scan(const char *dir, int depth, char (*out)[256], int *n, int max) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *n < max) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        size_t dl = strlen(dir);
        snprintf(path, sizeof(path), "%s%s%s", dir, (dl && dir[dl - 1] == '/') ? "" : "/", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 2 && strcasecmp(e->d_name, "gcbrowser") != 0 && strcasecmp(e->d_name, "webcache") != 0) scan(path, depth + 1, out, n, max);
        } else if (ends_dol(e->d_name) && strlen(path) < 256) {
            snprintf(out[(*n)++], 256, "%s", path);
        }
    }
    closedir(d);
}

static int cmp_path(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

int dol_find(const char *root, char (*out)[256], int max) {
    int n = 0;
    scan(root, 0, out, &n, max);
    qsort(out, (size_t)n, 256, cmp_path);
    return n;
}

/* ------------------------------------------------------------ launching */

#ifdef GEKKO
#include <gccore.h>
#include "sound.h"
#include "midi.h"
#include <fat.h>
#include <ogc/irq.h>
#include "storage.h"

/* exit.log in the data folder: each step of starting another program is
 * written down as it happens, so if it ever stops on a black screen the last
 * line shows where (the file is replaced on every exit). */
static void exit_log(bool first, const char *fmt, ...) {
    char path[400];
    storage_path(path, sizeof(path), "exit.log");
    FILE *f = fopen(path, first ? "w" : "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* The copier. It runs after everything else has stopped and while the memory
 * holding the browser's own code is being overwritten, so it must not call any
 * other function or touch any global: it only uses its argument, and it is
 * copied to the top of memory before it runs. */
__attribute__((section(".text.dolstub"), noinline, used, optimize("no-tree-loop-distribute-patterns")))
static void copier(DolPlan *p) {
    for (int i = 0; i < DOL_SECTIONS; i++) {
        uint32_t n = p->size[i];
        if (!n) continue;
        volatile unsigned char *dst = (volatile unsigned char *)p->addr[i];
        const volatile unsigned char *src = (const volatile unsigned char *)(p->image + p->off[i]);
        for (uint32_t k = 0; k < n; k++) dst[k] = src[k];
        for (uint32_t a = p->addr[i] & ~31u; a < p->addr[i] + n; a += 32)
            __asm__ volatile("dcbf 0,%0\n\ticbi 0,%0" : : "r"(a) : "memory");
    }
    if (p->bss_size) {
        volatile unsigned char *b = (volatile unsigned char *)p->bss_addr;
        for (uint32_t k = 0; k < p->bss_size; k++) b[k] = 0;
        for (uint32_t a = p->bss_addr & ~31u; a < p->bss_addr + p->bss_size; a += 32)
            __asm__ volatile("dcbf 0,%0" : : "r"(a) : "memory");
    }
    __asm__ volatile("sync\n\tisync\n\tmtctr %0\n\tbctr" : : "r"(p->entry) : "memory");
    for (;;) {}
}

bool dol_launch(const char *path, char *err, size_t errlen) {
    size_t len;
    unsigned char *file = read_file(path, &len, err, errlen);
    if (!file) return false;
    DolPlan plan;
    uint32_t prog_end;
    if (!parse(file, len, &plan, &prog_end, err, errlen)) { free(file); return false; }

    /* the copier is a few hundred bytes; copy a generous fixed block so its
     * exact size (which the compiler doesn't promise to expose) doesn't matter */
    const size_t stub_len = 2048;

    /* top of memory: [image][plan][copier][stack] just below MEM_HI */
    uint32_t top = MEM_HI - 0x1000;                    /* leave a little room at the very top */
    uint32_t stack_top = top & ~31u;
    uint32_t stub_at = (stack_top - 0x2000 - stub_len) & ~31u;   /* 8 KB of stack above it */
    uint32_t plan_at = (stub_at - sizeof(DolPlan) - 32) & ~31u;
    uint32_t image_at = (plan_at - len - 32) & ~31u;
    if (image_at < ((prog_end + 31) & ~31u)) {
        free(file);
        snprintf(err, errlen, "That program is too large to start from the browser.");
        return false;
    }
    plan.image = image_at;
    exit_log(true, "starting %s (%u bytes) file at %08x image at %08x, program ends %08x, entry %08x",
             path, (unsigned)len, (unsigned)(uintptr_t)file, (unsigned)image_at, (unsigned)prog_end, (unsigned)plan.entry);

    /* ---- point of no return: shut everything down ---- */
    for (int c = 0; c < 4; c++) PAD_ControlMotor(c, PAD_MOTOR_STOP);
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    exit_log(false, "screen off, stopping music");
    midi_shutdown();
    exit_log(false, "music stopped, stopping sound");
    sound_shutdown();
    exit_log(false, "sound stopped, stopping graphics");
    GX_AbortFrame();
    exit_log(false, "graphics stopped, closing SD card and jumping");
    fatUnmount("sd");
    /* the standard libogc hand-over, as other GameCube loaders do it: stops
     * the sound processor and every device, switches off the locked cache,
     * removes the exception handlers, ends the other threads and clears the
     * low-memory system values, so the next program starts from a clean
     * console (newer libogc2 builds leave more running than before). */
    SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);
    IRQ_Disable();   /* no more interrupts or thread switches from here on */

    /* move everything up (the file buffer may overlap the destination: memmove) */
    memmove((void *)image_at, file, len);
    memcpy((void *)plan_at, &plan, sizeof(plan));
    memcpy((void *)stub_at, (const void *)copier, stub_len);
    DCFlushRange((void *)image_at, (u32)(stack_top - image_at));
    ICInvalidateRange((void *)stub_at, (u32)stub_len);

    /* switch to the new stack and jump into the copied copier (never returns) */
    __asm__ volatile(
        "mr 1,%0\n\t"
        "mr 3,%1\n\t"
        "mtctr %2\n\t"
        "bctr"
        : : "r"(stack_top - 64), "r"(plan_at), "r"(stub_at) : "memory");
    for (;;) {}
}

void system_restart(void) {
    for (int c = 0; c < 4; c++) PAD_ControlMotor(c, PAD_MOTOR_STOP);
    fatUnmount("sd");
    SYS_ResetSystem(SYS_HOTRESET, 0, FALSE);
}
#else
bool dol_launch(const char *path, char *err, size_t errlen) {
    if (!dol_check(path, err, errlen)) return false;
    snprintf(err, errlen, "Launching programs only works on a GameCube.");
    return false;
}
void system_restart(void) {}
#endif
