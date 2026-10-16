/* storage.c - mounts the SD card through libfat and picks the data folder.
 * The SD2SP2 sits in Serial Port 2; SD Gecko adapters in memory card slots
 * A/B are tried as fallbacks. See storage.h for how the folder is chosen. */
#include "storage.h"
#include <fat.h>
#include <sdcard/gcsd.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool mounted;
static const char *device = "none";
static char data_dir[320] = STORAGE_ROOT_DIR;
static char prog_dir[300];      /* "" = unknown; "sd:" = root of the card */
static char prog_file[300];     /* the .dol itself, "" = unknown */
static char moved_from[320];

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && !S_ISDIR(st.st_mode);
}

static bool has_settings(const char *dir) {
    char p[400];
    snprintf(p, sizeof(p), "%s/settings.ini", dir);
    if (is_file(p)) return true;
    snprintf(p, sizeof(p), "%s/settings.tmp", dir);
    return is_file(p);
}

/* argv[0] as given by Swiss or another loader, e.g. "sd:/apps/cb/cubebrowser.dol"
 * (the device name may differ). We only trust it if that file really is on
 * our SD card. */
static void find_program_dir(const char *argv0) {
    prog_dir[0] = 0;
    prog_file[0] = 0;
    if (!argv0 || !*argv0) return;
    const char *colon = strstr(argv0, ":/");
    const char *path = colon ? colon + 1 : argv0;
    if (path[0] != '/') return;
    char full[300];
    if (snprintf(full, sizeof(full), "sd:%s", path) >= (int)sizeof(full)) return;
    if (!is_file(full)) return;
    snprintf(prog_file, sizeof(prog_file), "%s", full);
    char *slash = strrchr(full, '/');
    *slash = 0;                       /* "sd:/apps/cb" or "sd:" */
    snprintf(prog_dir, sizeof(prog_dir), "%s", full);
}

/* When the .dol's folder is unknown: look one level down from the root and
 * from sd:/apps for a gcbrowser folder (preferring one with settings). */
static bool scan_for_data_dir(char *out, size_t cap) {
    static const char *const bases[] = { "sd:/apps", "sd:" };
    char first[320] = "";
    for (int b = 0; b < 2; b++) {
        char base_open[64];
        snprintf(base_open, sizeof(base_open), "%s/", bases[b]);
        DIR *d = opendir(base_open);
        if (!d) continue;
        struct dirent *e;
        int seen = 0;
        while ((e = readdir(d)) != NULL && seen++ < 400) {
            if (e->d_name[0] == '.') continue;
            char cand[320];
            if (snprintf(cand, sizeof(cand), "%s/%s/gcbrowser", bases[b], e->d_name) >= (int)sizeof(cand)) continue;
            if (!is_dir(cand)) continue;
            if (has_settings(cand)) { closedir(d); snprintf(out, cap, "%s", cand); return true; }
            if (!first[0]) snprintf(first, sizeof(first), "%s", cand);
        }
        closedir(d);
    }
    if (first[0]) { snprintf(out, cap, "%s", first); return true; }
    return false;
}

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return false; }
    char *buf = malloc(32 * 1024);
    bool ok = buf != NULL;
    size_t n;
    while (ok && (n = fread(buf, 1, 32 * 1024, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) ok = false;
    if (ferror(in)) ok = false;
    free(buf);
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) remove(to);
    return ok;
}

/* Moves the plain files of `from` into `to` (created if needed), then
 * removes `from` if it ended up empty. With overwrite false, files already
 * in `to` are kept and the old copies dropped. Returns files that couldn't
 * be moved. */
static int move_files(const char *from, const char *to, bool overwrite) {
    mkdir(to, 0777);
    char (*names)[256] = malloc(sizeof(*names) * 64);
    if (!names) return 1;
    int count = 0;
    char open_path[330];
    snprintf(open_path, sizeof(open_path), "%s/", from);
    DIR *d = opendir(open_path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && count < 64) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) >= 256) continue;
            snprintf(names[count++], 256, "%s", e->d_name);
        }
        closedir(d);
    }
    int failed = 0;
    for (int i = 0; i < count; i++) {
        char src[600], dst[600];
        snprintf(src, sizeof(src), "%s/%s", from, names[i]);
        snprintf(dst, sizeof(dst), "%s/%s", to, names[i]);
        if (!is_file(src)) continue;                      /* leave sub-folders alone */
        if (is_file(dst)) {
            if (!overwrite) { remove(src); continue; }
            remove(dst);
        }
        if (rename(src, dst) == 0) continue;
        if (copy_file(src, dst)) remove(src);
        else failed++;
    }
    free(names);
    rmdir(from);
    return failed;
}

bool storage_init(const char *argv0) {
    struct { const DISC_INTERFACE *io; const char *name; } devs[] = {
        { &__io_gcsd2, "SD2SP2" },
        { &__io_gcsdb, "SD Gecko (slot B)" },
        { &__io_gcsda, "SD Gecko (slot A)" },
    };
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        if (fatMountSimple("sd", devs[i].io)) {
            mounted = true;
            device = devs[i].name;
            break;
        }
    }
    if (!mounted) return false;

    find_program_dir(argv0);
    char chosen[320] = "";
    if (prog_dir[0]) {
        char p[320];
        snprintf(p, sizeof(p), "%s/gcbrowser", prog_dir);
        if (is_dir(p)) snprintf(chosen, sizeof(chosen), "%s", p);
    } else {
        scan_for_data_dir(chosen, sizeof(chosen));
    }
    if (!chosen[0]) snprintf(chosen, sizeof(chosen), "%s", STORAGE_ROOT_DIR);
    snprintf(data_dir, sizeof(data_dir), "%s", chosen);
    mkdir(data_dir, 0777);

    /* settings from an older version at the root: bring them along */
    if (strcmp(data_dir, STORAGE_ROOT_DIR) != 0 && !has_settings(data_dir) && has_settings(STORAGE_ROOT_DIR)) {
        move_files(STORAGE_ROOT_DIR, data_dir, false);
        snprintf(moved_from, sizeof(moved_from), "%s", STORAGE_ROOT_DIR);
    }
    return true;
}

bool storage_ok(void) { return mounted; }
const char *storage_device_name(void) { return device; }
const char *storage_dir(void) { return data_dir; }
const char *storage_program_dir(void) { return prog_dir[0] ? prog_dir : NULL; }
const char *storage_program_file(void) { return prog_file[0] ? prog_file : NULL; }
bool storage_dir_is_root(void) { return strcmp(data_dir, STORAGE_ROOT_DIR) == 0; }
const char *storage_moved_from(void) { return moved_from[0] ? moved_from : NULL; }

void storage_path(char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s/%s", data_dir, name);
}

bool storage_move_data(bool to_root, char *msg, size_t cap) {
    if (!mounted) { snprintf(msg, cap, "No SD card found"); return false; }
    char target[320];
    if (to_root) {
        snprintf(target, sizeof(target), "%s", STORAGE_ROOT_DIR);
    } else {
        if (!prog_dir[0]) {
            snprintf(msg, cap, "Swiss didn't say which folder CubeBrowser is in. Move the gcbrowser folder on a PC.");
            return false;
        }
        snprintf(target, sizeof(target), "%s/gcbrowser", prog_dir);
    }
    if (strcmp(target, data_dir) == 0) { snprintf(msg, cap, "Data is already in %s", target); return false; }
    char old[320];
    snprintf(old, sizeof(old), "%s", data_dir);
    int failed = move_files(old, target, true);
    if (failed) {
        /* some files are still in the old folder: keep using it */
        snprintf(msg, cap, "Couldn't move %d file%s. Nothing was changed.", failed, failed == 1 ? "" : "s");
        move_files(target, old, true);
        return false;
    }
    if (is_dir(old)) {
        /* something (a sub-folder) was left behind: rename the old folder so
         * it isn't picked again at the next start */
        char aside[340];
        snprintf(aside, sizeof(aside), "%s-old", old);
        rename(old, aside);
    }
    snprintf(data_dir, sizeof(data_dir), "%s", target);
    snprintf(msg, cap, "Data moved to %s", target);
    return true;
}

char *storage_read(const char *path, size_t *len_out) {
    if (!mounted) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > 8 * 1024 * 1024) { fclose(f); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = 0;
    if (len_out) *len_out = got;
    return buf;
}

/* Write to a temp file, then swap it in, so a power cut never leaves a
 * half-written file in place. settings_load() also checks for the temp file. */
bool storage_write_atomic(const char *path, const char *tmp_path, const void *data, size_t len) {
    if (!mounted) return false;
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    fclose(f);
    if (!ok) { remove(tmp_path); return false; }
    remove(path);
    return rename(tmp_path, path) == 0;
}
