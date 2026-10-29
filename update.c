/* update.c - see update.h */
#include "update.h"
#include "version.h"
#include "fetch.h"
#include "http.h"
#include "download.h"
#include "unzip.h"
#include "dolload.h"
#include "storage.h"
#include "settings.h"
#include "cookies.h"
#include "diag.h"
#include "ui.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile UpdateState state = UPD_NONE;
static volatile UpdateStep step;
static volatile bool want_check, want_install, quiet_check;
static char latest[32];               /* "1.9" */
static char asset_url[URL_MAX];
static char notes[3000];
static char err_msg[256];
static char target[300];              /* the .dol being replaced */

UpdateState update_state(void) { return state; }
UpdateStep update_step(void) { return step; }
const char *update_latest_version(void) { return latest; }
const char *update_notes(void) { return notes; }
const char *update_error(void) { return err_msg; }
const char *update_target(void) { return target; }

/* ------------------------------------------------------------ versions */

/* Reads up to 4 dotted numbers from s ("v1.10.2" -> 1,10,2,0). Returns how many. */
static int parse_version(const char *s, int out[4]) {
    memset(out, 0, sizeof(int) * 4);
    while (*s && !isdigit((unsigned char)*s)) s++;
    int n = 0;
    while (*s && n < 4 && isdigit((unsigned char)*s)) {
        out[n++] = atoi(s);
        while (isdigit((unsigned char)*s)) s++;
        if (*s != '.') break;
        s++;
    }
    return n;
}

static int compare_versions(const int a[4], const int b[4]) {
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* ------------------------------------------------------------ tiny JSON reading
 * Only what the GitHub "latest release" reply needs: a few top-level strings
 * and the asset download links. */

/* Copies the JSON string starting at p (just after the opening quote),
 * decoding escapes. Returns a pointer after the closing quote. */
static const char *json_string(const char *p, char *out, size_t cap) {
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = ' '; break;
            case 'u': {   /* \uXXXX: keep ASCII, show others as '?' */
                unsigned v = 0;
                for (int i = 0; i < 4 && isxdigit((unsigned char)*p); i++, p++)
                    v = v * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                c = (v >= 32 && v < 127) ? (char)v : (v == 0x2019 || v == 0x2018 ? '\'' : (v == 0x201c || v == 0x201d ? '"' : (v == 0x2013 || v == 0x2014 ? '-' : '?')));
                break;
            }
            default: c = e; break;   /* \" \\ \/ */
            }
        } else if ((unsigned char)c >= 0x80) {
            /* UTF-8 multibyte: skip continuation bytes, show one '?' (fonts are ASCII) */
            while (((unsigned char)*p & 0xC0) == 0x80) p++;
            c = '?';
        }
        if (n + 1 < cap) out[n++] = c;
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return *p == '"' ? p + 1 : p;
}

/* Finds "key": "value" (first occurrence at or after from). */
static bool json_find_string(const char *json, const char *key, char *out, size_t cap, const char **after) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '"') return false;
    p = json_string(p + 1, out, cap);
    if (after) *after = p;
    return true;
}

/* Release notes are Markdown: tidy the common bits for plain display. */
static void tidy_notes(char *s) {
    char *w = s;
    bool line_start = true;
    for (char *r = s; *r; r++) {
        if (*r == '\r') continue;
        if (line_start) {
            while (*r == '#') r++;                        /* headings */
            while (*r == ' ') r++;
            if ((*r == '*' || *r == '-') && r[1] == ' ') { *w++ = '-'; r++; }   /* bullets */
        }
        if (!*r) break;
        if (*r == '*' || *r == '`' || (*r == '_' && r[1] == '_')) { if (*r == '_') r++; line_start = false; continue; }
        *w++ = *r;
        line_start = (*r == '\n');
    }
    *w = 0;
    while (w > s && (w[-1] == '\n' || w[-1] == ' ')) *--w = 0;
}

/* Shared with the Swiss updater (swissupd.c). */
bool update_json_string(const char *json, const char *key, char *out, size_t cap, const char **after) {
    return json_find_string(json, key, out, cap, after);
}
void update_tidy_notes(char *s) { tidy_notes(s); }

/* ------------------------------------------------------------ worker: check */

void update_worker_check(volatile int *cancel) {
    char url[200];
#ifdef UPDATE_TEST_API
    snprintf(url, sizeof(url), "%s", UPDATE_TEST_API);   /* PC tests only */
#else
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", UPDATE_REPO);
#endif
    HttpHooks hooks = { cancel, NULL, NULL, NULL, true, NULL };
    HttpResult r;
    diag_note(DIAG_WORKER, "checking for updates");
    if (!http_fetch(url, NULL, "application/vnd.github+json", &r, &hooks)) {
        snprintf(err_msg, sizeof(err_msg), "%s", r.error[0] ? r.error : "GitHub couldn't be reached.");
        http_result_free(&r);
        state = UPD_CHECK_FAILED;
        return;
    }
    if (r.status == 404) {
        snprintf(err_msg, sizeof(err_msg), "No releases have been published yet.");
        http_result_free(&r);
        state = UPD_CHECK_FAILED;
        return;
    }
    if (r.status == 403 || r.status == 429) {
        snprintf(err_msg, sizeof(err_msg), "GitHub is limiting checks from your network. Try again in an hour.");
        http_result_free(&r);
        state = UPD_CHECK_FAILED;
        return;
    }
    if (r.status != 200 || !r.body) {
        snprintf(err_msg, sizeof(err_msg), "GitHub answered with error %d.", r.status);
        http_result_free(&r);
        state = UPD_CHECK_FAILED;
        return;
    }
    char tag[64] = "";
    json_find_string(r.body, "tag_name", tag, sizeof(tag), NULL);
    int have[4], got[4];
    parse_version(CB_VERSION, have);
    if (parse_version(tag, got) == 0) {
        /* not a version number: nothing to compare, so nothing to offer */
        snprintf(latest, sizeof(latest), "%s", tag);
        http_result_free(&r);
        state = UPD_LATEST;
        return;
    }
    const char *t = tag;
    while (*t && !isdigit((unsigned char)*t)) t++;
    snprintf(latest, sizeof(latest), "%s", t);
    notes[0] = 0;
    json_find_string(r.body, "body", notes, sizeof(notes), NULL);
    tidy_notes(notes);
    /* the download: the first .zip asset (or a bare .dol) */
    asset_url[0] = 0;
    const char *p = r.body;
    char link[URL_MAX];
    while (json_find_string(p, "browser_download_url", link, sizeof(link), &p)) {
        size_t n = strlen(link);
        if (n > 4 && !strcasecmp(link + n - 4, ".zip")) { snprintf(asset_url, sizeof(asset_url), "%s", link); break; }
        if (n > 4 && !strcasecmp(link + n - 4, ".dol") && !asset_url[0]) snprintf(asset_url, sizeof(asset_url), "%s", link);
    }
    http_result_free(&r);
    if (compare_versions(got, have) <= 0) { state = UPD_LATEST; return; }
    if (!asset_url[0]) {
        snprintf(err_msg, sizeof(err_msg), "Version %s has no download attached yet.", latest);
        state = UPD_CHECK_FAILED;
        return;
    }
    state = UPD_AVAILABLE;
}

/* ------------------------------------------------------------ worker: install */

static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && !S_ISDIR(st.st_mode);
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

/* Deletes a folder and everything in it (a few levels deep at most). */
static void remove_tree(const char *path, int depth) {
    char open_path[330];
    snprintf(open_path, sizeof(open_path), "%s/", path);
    DIR *d = opendir(open_path);
    if (d) {
        char (*names)[256] = malloc(sizeof(*names) * 64);
        int count = 0;
        struct dirent *e;
        while (names && (e = readdir(d)) != NULL && count < 64)
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) snprintf(names[count++], 256, "%s", e->d_name);
        closedir(d);
        for (int i = 0; i < count; i++) {
            char sub[600];
            snprintf(sub, sizeof(sub), "%s/%s", path, names[i]);
            if (is_file(sub)) remove(sub);
            else if (depth < 4) remove_tree(sub, depth + 1);
        }
        free(names);
    }
    rmdir(path);
}

/* Where launchers and Dolphin Depot install CubeBrowser on the card. The
 * release zip holds that folder: cubebrowser/ with default.dol, opening.bnr
 * and the gcbrowser folder (older releases: cubebrowser.dol and gcbrowser
 * at the top of the zip). */
#define APP_DIR "apps/cubebrowser"

/* The folder in the unpacked update that holds the release's files. */
static void release_dir(const char *dir, char *out, size_t cap) {
    char p[400];
    snprintf(p, sizeof(p), "%s/cubebrowser/default.dol", dir);
    if (is_file(p)) snprintf(out, cap, "%s/cubebrowser", dir);
    else snprintf(out, cap, "%s", dir);
}

/* Any .dol in dir or the folders below it (a few levels), preferring default.dol. */
static bool find_any_dol(const char *dir, int depth, char *out, size_t cap) {
    char open_path[330];
    snprintf(open_path, sizeof(open_path), "%s/", dir);
    DIR *d = opendir(open_path);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    char sub[8][330];
    int nsub = 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[330];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcasecmp(e->d_name + n - 4, ".dol") && is_file(p)) {
            snprintf(out, cap, "%s", p);
            found = true;
        } else if (depth < 3 && nsub < 8 && !is_file(p)) {
            snprintf(sub[nsub++], sizeof(sub[0]), "%s", p);
        }
    }
    closedir(d);
    for (int i = 0; !found && i < nsub; i++) found = find_any_dol(sub[i], depth + 1, out, cap);
    return found;
}

/* The .dol inside the unpacked update: cubebrowser/default.dol, default.dol
 * or cubebrowser.dol at the top, or failing those any .dol in it. */
static bool find_new_dol(const char *dir, char *out, size_t cap) {
    snprintf(out, cap, "%s/cubebrowser/default.dol", dir);
    if (is_file(out)) return true;
    snprintf(out, cap, "%s/default.dol", dir);
    if (is_file(out)) return true;
    snprintf(out, cap, "%s/cubebrowser.dol", dir);
    if (is_file(out)) return true;
    return find_any_dol(dir, 0, out, cap);
}

/* Which .dol to replace: the one we were started from, if Swiss said so;
 * otherwise the only cubebrowser*.dol on the card, or apps/cubebrowser/default.dol. */
static bool find_target(char *out, size_t cap) {
    const char *pf = storage_program_file();
    if (pf) { snprintf(out, cap, "%s", pf); return true; }
    static char found[48][256];
    int n = dol_find("sd:/", found, 48), match = -1, matches = 0;
    for (int i = 0; i < n; i++) {
        const char *name = strrchr(found[i], '/');
        name = name ? name + 1 : found[i];
        if (!strncasecmp(name, "cubebrowser", 11) && !strstr(name, ".old") && !strstr(name, ".new")) { match = i; matches++; }
    }
    if (matches == 1) { snprintf(out, cap, "%s", found[match]); return true; }
    /* the way launchers and Dolphin Depot install it */
    snprintf(out, cap, "sd:/" APP_DIR "/default.dol");
    return is_file(out);
}

static void install_failed(const char *msg) {
    snprintf(err_msg, sizeof(err_msg), "%s", msg);
    state = UPD_FAILED;
}

void update_worker_install(volatile int *cancel) {
    char zip[400], dir[400], newdol[600], err[256];
    storage_path(zip, sizeof(zip), "update.zip");
    storage_path(dir, sizeof(dir), "update");
    if (!find_target(target, sizeof(target))) {
        install_failed("Couldn't tell which file is CubeBrowser on the SD card. Download the zip from GitHub on a PC instead.");
        return;
    }
    /* 1. download (a leftover partial file from an older attempt could be a different version) */
    step = UPD_STEP_DOWNLOAD;
    char part[420];
    snprintf(part, sizeof(part), "%s.part", zip);
    remove(part);
    remove(zip);
    diag_note(DIAG_WORKER, "update: downloading %.120s", asset_url);
    DownloadProgress *dp = (DownloadProgress *)fetch_download_progress();
    size_t ul = strlen(asset_url);
    bool bare_dol = ul > 4 && !strcasecmp(asset_url + ul - 4, ".dol");
    if (bare_dol) storage_path(zip, sizeof(zip), "update.dol");
    if (!download_to_file(asset_url, zip, dp, cancel, err, sizeof(err))) {
        remove(part);
        if (*cancel) install_failed("Stopped. Nothing was changed.");
        else {
            char m[300];
            snprintf(m, sizeof(m), "%s Nothing was changed.", err);
            install_failed(m);
        }
        return;
    }
    /* 2. unpack and check */
    step = UPD_STEP_UNPACK;
    remove_tree(dir, 0);
    if (bare_dol) {
        snprintf(newdol, sizeof(newdol), "%s", zip);
    } else {
        UnzipProgress *up = (UnzipProgress *)fetch_unzip_progress();
        if (!unzip_file(zip, dir, up, cancel, err, sizeof(err))) {
            remove(zip);
            remove_tree(dir, 0);
            char m[300];
            snprintf(m, sizeof(m), "The download couldn't be unpacked (%s). Nothing was changed.", err);
            install_failed(m);
            return;
        }
        if (!find_new_dol(dir, newdol, sizeof(newdol))) {
            remove(zip);
            remove_tree(dir, 0);
            install_failed("The download didn't contain a .dol file. Nothing was changed.");
            return;
        }
    }
    if (!dol_check(newdol, err, sizeof(err))) {
        char m[300];
        snprintf(m, sizeof(m), "The downloaded program looks damaged (%s). Nothing was changed.", err);
        remove(zip);
        remove_tree(dir, 0);
        install_failed(m);
        return;
    }
    /* 3. replace: new -> <target>.new, <target> -> <target>.old, <target>.new -> <target> */
    step = UPD_STEP_REPLACE;
    char tnew[320], told[320];
    snprintf(tnew, sizeof(tnew), "%s.new", target);
    snprintf(told, sizeof(told), "%s.old", target);
    remove(tnew);
    if (rename(newdol, tnew) != 0 && !copy_file(newdol, tnew)) {
        remove(zip);
        remove_tree(dir, 0);
        install_failed("Couldn't write to the SD card (is it full or locked?). Nothing was changed.");
        return;
    }
    remove(told);
    bool had_old = is_file(target);
    if (had_old && rename(target, told) != 0) {
        remove(tnew);
        remove(zip);
        remove_tree(dir, 0);
        install_failed("Couldn't replace the old version. Nothing was changed.");
        return;
    }
    if (rename(tnew, target) != 0) {
        if (had_old) rename(told, target);   /* put the old one back */
        remove(tnew);
        remove(zip);
        remove_tree(dir, 0);
        install_failed("Couldn't put the new version in place. The old one was kept.");
        return;
    }
    /* certificates from the release (settings, bookmarks and cookies are left alone) */
    char rel[420];
    release_dir(dir, rel, sizeof(rel));
    char newca[600], ca[400], catmp[420];
    snprintf(newca, sizeof(newca), "%s/gcbrowser/cacert.pem", rel);
    storage_path(ca, sizeof(ca), "cacert.pem");
    snprintf(catmp, sizeof(catmp), "%s.tmp", ca);
    if (!bare_dol && is_file(newca) && copy_file(newca, catmp)) {
        remove(ca);
        rename(catmp, ca);
    }
    /* the banner launchers show (opening.bnr), when CubeBrowser lives in a
     * folder of its own; never at the root of the card, where it would be
     * taken for everything's banner */
    char newbnr[600], bnr[340];
    snprintf(newbnr, sizeof(newbnr), "%s/opening.bnr", rel);
    const char *slash = strrchr(target, '/');
    if (!bare_dol && is_file(newbnr) && slash && slash - target > 3) {   /* not "sd:/x.dol" */
        snprintf(bnr, sizeof(bnr), "%.*s/opening.bnr", (int)(slash - target), target);
        char bnrtmp[350];
        snprintf(bnrtmp, sizeof(bnrtmp), "%s.tmp", bnr);
        if (copy_file(newbnr, bnrtmp)) { remove(bnr); rename(bnrtmp, bnr); }
    }
    remove(zip);
    remove_tree(dir, 0);
    diag_note(DIAG_WORKER, "update: installed %s as %s", latest, target);
    state = UPD_INSTALLED;
}

/* ------------------------------------------------------------ UI thread */

void update_request_check(bool quiet) {
    if (state == UPD_CHECKING || state == UPD_INSTALLING || state == UPD_INSTALLED) return;
    quiet_check = quiet;
    want_check = true;
    err_msg[0] = 0;
    state = UPD_CHECKING;
}

void update_request_install(void) {
    if (state != UPD_AVAILABLE && state != UPD_FAILED) return;
    if (!storage_ok()) { snprintf(err_msg, sizeof(err_msg), "No SD card was found."); state = UPD_FAILED; return; }
    want_install = true;
    err_msg[0] = 0;
    step = UPD_STEP_DOWNLOAD;
    state = UPD_INSTALLING;
}

void update_poll(void) {
    /* collect a finished job */
    if (fetch_take_update()) {
        if (state == UPD_AVAILABLE && quiet_check) {
            char m[96];
            snprintf(m, sizeof(m), "CubeBrowser %s is available - see the start page", latest);
            ui_toast(m);
        }
        if (state == UPD_INSTALLED) {
            char m[96];
            snprintf(m, sizeof(m), "CubeBrowser %s is installed - restart to use it", latest);
            ui_toast(m);
        }
        if (state == UPD_CHECK_FAILED && quiet_check) state = UPD_NONE;   /* quiet: just try again another time */
        quiet_check = false;
        return;
    }
    /* start a queued one once the worker is free */
    if ((want_check || want_install) && fetch_state() == FETCH_IDLE && fetch_net_ready()) {
        if (want_install) { if (fetch_start_update(true)) want_install = false; }
        else if (fetch_start_update(false)) want_check = false;
    }
}

bool update_restart(char *err, size_t errlen) {
    if (state != UPD_INSTALLED) { snprintf(err, errlen, "Nothing to restart into yet."); return false; }
    settings_save();
    cookies_save_if_needed();
    FILE *f = fopen(RESTART_NOTE, "wb");   /* so the new version knows where it lives (storage.c) */
    if (f) { fputs(target, f); fclose(f); }
    bool ok = dol_launch(target, err, errlen);
    remove(RESTART_NOTE);                  /* only reached if it couldn't start */
    return ok;
}
