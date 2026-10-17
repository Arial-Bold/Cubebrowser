/* swissupd.c - see swissupd.h */
#include "swissupd.h"
#include "fetch.h"
#include "http.h"
#include "download.h"
#include "unzip.h"
#include "dolload.h"
#include "storage.h"
#include "settings.h"
#include "diag.h"
#include "ui.h"
#include "7z.h"
#include "7zCrc.h"
#include "LzmaDec.h"
#include "xz_config.h"   /* xz.h with this build's options (CRC64 on) */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static volatile UpdateState state = UPD_NONE;
static volatile UpdateStep step;
static volatile bool want_check, want_install, just_installed;
static char latest[24];          /* "r2119" */
static int latest_rev;
static char installed[24];
static char url_7z[URL_MAX], url_txz[URL_MAX];   /* the release's .7z and .tar.xz (same contents) */
static char notes[3000];
static char err_msg[256];
static char target[300];         /* the Swiss .dol being replaced */
static char new_path[300];       /* where the new one goes (target, or renamed for the new number) */

UpdateState swiss_state(void) { return state; }
UpdateStep swiss_step(void) { return step; }
const char *swiss_latest(void) { return latest; }
const char *swiss_notes(void) { return notes; }
const char *swiss_error(void) { return err_msg; }
const char *swiss_target(void) { return state == UPD_INSTALLED ? new_path : target; }

/* ------------------------------------------------------------ helpers */

static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && !S_ISDIR(st.st_mode);
}

/* "swiss_r2092.dol", "v0.6r2119" -> 2092 / 2119; 0 if there's no rNNNN. */
static int rev_in(const char *s) {
    for (const char *p = s; *p; p++)
        if ((*p == 'r' || *p == 'R') && isdigit((unsigned char)p[1]) && (p == s || !isalpha((unsigned char)p[-1]))) {
            int v = atoi(p + 1);
            if (v >= 100) return v;
        }
    return 0;
}

bool swiss_find_dol(char *out, size_t cap) {
    char err[200];
    if (g_set.swiss_dol[0] && dol_check(g_set.swiss_dol, err, sizeof(err))) {
        snprintf(out, cap, "%s", g_set.swiss_dol);
        return true;
    }
    static char found[48][256];
    int n = storage_ok() ? dol_find("sd:/", found, 48) : 0;
    for (int i = 0; i < n; i++) {
        const char *name = strrchr(found[i], '/');
        name = name ? name + 1 : found[i];
        if (!strncasecmp(name, "swiss", 5) && !strstr(name, ".old") && dol_check(found[i], err, sizeof(err))) {
            snprintf(out, cap, "%s", found[i]);
            return true;
        }
    }
    return false;
}

/* What's installed at path, as far as we can tell. */
static void work_out_installed(const char *path) {
    int r = 0;
    if (g_set.swiss_rev > 0 && !strcmp(g_set.swiss_rev_path, path)) r = g_set.swiss_rev;
    else {
        const char *name = strrchr(path, '/');
        r = rev_in(name ? name + 1 : path);
    }
    if (r > 0) snprintf(installed, sizeof(installed), "r%d", r);
    else installed[0] = 0;
}

const char *swiss_installed(void) { return installed; }

/* Where the new Swiss will be saved, from Settings > Updates > Save new Swiss
 * as: its present name (a swiss_rNNNN.dol gets the new number), the numbered
 * name, swiss.dol beside it, or boot.dol / ipl.dol at the root of the card
 * (where boot loaders look for them). */
void swiss_destination(char *out, size_t cap) {
    char dir[300] = "sd:/";
    const char *slash = target[0] ? strrchr(target, '/') : NULL;
    const char *base = slash ? slash + 1 : target;
    if (slash) snprintf(dir, sizeof(dir), "%.*s", (int)(slash + 1 - target), target);
    int rev = latest_rev > 0 ? latest_rev : 0;
    switch (g_set.swiss_name) {
    default:
    case SWISS_NAME_KEEP:
        if (!target[0]) snprintf(out, cap, "sd:/swiss.dol");
        else if (!strncasecmp(base, "swiss_r", 7) && rev_in(base) > 0 && rev) snprintf(out, cap, "%sswiss_r%d.dol", dir, rev);
        else snprintf(out, cap, "%s", target);
        break;
    case SWISS_NAME_NUMBERED: snprintf(out, cap, "%sswiss_r%d.dol", dir, rev); break;
    case SWISS_NAME_SWISS: snprintf(out, cap, "%sswiss.dol", dir); break;
    case SWISS_NAME_BOOT: snprintf(out, cap, "sd:/boot.dol"); break;
    case SWISS_NAME_IPL: snprintf(out, cap, "sd:/ipl.dol"); break;
    }
}

/* ------------------------------------------------------------ worker: check */

void swiss_worker_check(volatile int *cancel) {
    char url[200];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", SWISS_REPO);
    HttpHooks hooks = { cancel, NULL, NULL, NULL, true, NULL };
    HttpResult r;
    diag_note(DIAG_WORKER, "checking for Swiss updates");
    if (!http_fetch(url, NULL, "application/vnd.github+json", &r, &hooks)) {
        snprintf(err_msg, sizeof(err_msg), "%s", r.error[0] ? r.error : "GitHub couldn't be reached.");
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
    update_json_string(r.body, "tag_name", tag, sizeof(tag), NULL);
    latest_rev = rev_in(tag);
    snprintf(latest, sizeof(latest), "r%d", latest_rev);
    notes[0] = 0;
    update_json_string(r.body, "body", notes, sizeof(notes), NULL);
    update_tidy_notes(notes);
    /* the downloads: swiss_rNNNN.7z (smaller), and swiss_rNNNN.tar.xz as a fallback */
    url_7z[0] = url_txz[0] = 0;
    const char *p = r.body;
    char link[URL_MAX];
    while (update_json_string(p, "browser_download_url", link, sizeof(link), &p)) {
        const char *name = strrchr(link, '/');
        name = name ? name + 1 : link;
        size_t n = strlen(name);
        if (strncasecmp(name, "swiss_", 6)) continue;
        if (n > 3 && !strcasecmp(name + n - 3, ".7z")) snprintf(url_7z, sizeof(url_7z), "%s", link);
        else if (n > 7 && !strcasecmp(name + n - 7, ".tar.xz")) snprintf(url_txz, sizeof(url_txz), "%s", link);
    }
    http_result_free(&r);
    if (!latest_rev) {
        snprintf(err_msg, sizeof(err_msg), "Couldn't read the version of the latest Swiss release (\"%s\").", tag);
        state = UPD_CHECK_FAILED;
        return;
    }
    if (!swiss_find_dol(target, sizeof(target))) target[0] = 0;
    else work_out_installed(target);
    int have = installed[0] ? atoi(installed + 1) : 0;
    if (have >= latest_rev) { state = UPD_LATEST; return; }
    if (!url_7z[0] && !url_txz[0]) {
        snprintf(err_msg, sizeof(err_msg), "Swiss %s has no .7z download attached.", latest);
        state = UPD_CHECK_FAILED;
        return;
    }
    state = UPD_AVAILABLE;
}

/* ------------------------------------------------------------ worker: install */

/* Reads the .tar.xz and writes the one file wanted (the main Swiss .dol)
 * to out_path. Stops as soon as it has it. */
typedef struct {
    unsigned char hdr[512];
    size_t hdr_have;
    uint64_t file_left, pad_left;
    FILE *out;
    bool writing, done, ended, write_err;
} Tar;

static bool wanted_entry(const char *name) {
    size_t n = strlen(name);
    if (n < 5 || strcasecmp(name + n - 4, ".dol")) return false;
    if (strstr(name, "/Legacy/") || !strstr(name, "/DOL/")) return false;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    return !strncasecmp(base, "swiss_r", 7);
}

static uint64_t octal(const unsigned char *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n && p[i]; i++) {
        if (p[i] == ' ') continue;
        if (p[i] < '0' || p[i] > '7') break;
        v = v * 8 + (uint64_t)(p[i] - '0');
    }
    return v;
}

static void tar_feed(Tar *t, const unsigned char *d, size_t n, const char *out_path) {
    while (n > 0 && !t->done && !t->ended) {
        if (t->file_left > 0) {
            size_t k = n < t->file_left ? n : (size_t)t->file_left;
            if (t->writing && fwrite(d, 1, k, t->out) != k) t->write_err = true;
            t->file_left -= k;
            d += k;
            n -= k;
            if (t->file_left == 0 && t->writing) {
                if (fclose(t->out) != 0) t->write_err = true;
                t->out = NULL;
                t->writing = false;
                t->done = true;
            }
            continue;
        }
        if (t->pad_left > 0) {
            size_t k = n < t->pad_left ? n : (size_t)t->pad_left;
            t->pad_left -= k;
            d += k;
            n -= k;
            continue;
        }
        size_t k = 512 - t->hdr_have;
        if (k > n) k = n;
        memcpy(t->hdr + t->hdr_have, d, k);
        t->hdr_have += k;
        d += k;
        n -= k;
        if (t->hdr_have < 512) continue;
        t->hdr_have = 0;
        bool zero = true;
        for (int i = 0; i < 512 && zero; i++) if (t->hdr[i]) zero = false;
        if (zero) { t->ended = true; break; }
        char name[260];
        char pre[156], nm[101];
        memcpy(nm, t->hdr, 100);
        nm[100] = 0;
        memcpy(pre, t->hdr + 345, 155);
        pre[155] = 0;
        if (pre[0] && !memcmp(t->hdr + 257, "ustar", 5)) snprintf(name, sizeof(name), "%s/%s", pre, nm);
        else snprintf(name, sizeof(name), "%s", nm);
        uint64_t size = octal(t->hdr + 124, 12);
        char type = (char)t->hdr[156];
        t->file_left = size;
        t->pad_left = (512 - size % 512) % 512;
        if ((type == '0' || type == 0) && wanted_entry(name) && size > 0) {
            t->out = fopen(out_path, "wb");
            if (!t->out) { t->write_err = true; t->ended = true; break; }
            t->writing = true;
        }
    }
}

static bool extract_swiss_dol(const char *archive, const char *out_path, UnzipProgress *up, volatile int *cancel,
                              char *err, size_t errlen) {
    FILE *f = fopen(archive, "rb");
    if (!f) { snprintf(err, errlen, "couldn't open the download"); return false; }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    up->total = (uint64_t)total;
    up->done = 0;
    xz_crc32_init();
    xz_crc64_init();
    struct xz_dec *xd = xz_dec_init(XZ_DYNALLOC, 1u << 26);
    unsigned char *in = malloc(32768), *outb = malloc(32768);
    Tar *t = calloc(1, sizeof(Tar));
    bool ok = false;
    if (!xd || !in || !outb || !t) { snprintf(err, errlen, "not enough memory"); goto done; }
    struct xz_buf b = { in, 0, 0, outb, 0, 32768 };
    for (;;) {
        if (*cancel) { snprintf(err, errlen, "stopped"); break; }
        if (b.in_pos == b.in_size) {
            b.in_size = fread(in, 1, 32768, f);
            b.in_pos = 0;
            up->done += b.in_size;
        }
        enum xz_ret ret = xz_dec_run(xd, &b);
        if (b.out_pos) {
            tar_feed(t, outb, b.out_pos, out_path);
            b.out_pos = 0;
        }
        if (t->done || t->ended || t->write_err) break;
        if (ret == XZ_STREAM_END) break;
        if (ret == XZ_MEM_ERROR || ret == XZ_MEMLIMIT_ERROR) { snprintf(err, errlen, "not enough memory to unpack it"); break; }
        if (ret != XZ_OK) { snprintf(err, errlen, "the download is damaged"); break; }
        if (b.in_size == 0) { snprintf(err, errlen, "the download ended early"); break; }
    }
    if (t && t->write_err) snprintf(err, errlen, "couldn't write to the SD card");
    else if (t && t->done) ok = true;
    else if (t && t->ended && !err[0]) snprintf(err, errlen, "the Swiss .dol wasn't in the download");
done:
    if (t && t->out) { fclose(t->out); remove(out_path); }
    if (!ok) remove(out_path);
    free(t);
    free(in);
    free(outb);
    if (xd) xz_dec_end(xd);
    fclose(f);
    return ok;
}

/* ---- .7z: the Swiss .dol from a 7-Zip archive
 *
 * The archive is "solid": many files packed as one LZMA stream with a 32 MB
 * dictionary, more than the GameCube has. But the stream only needs to be
 * unpacked as far as the end of the .dol (about 9 MB in, as the files are
 * in alphabetical order), and LZMA never looks further back than what has
 * been unpacked, so a buffer that size is enough. */

#define SZ_WINDOW_MAX (14u * 1024u * 1024u)

static void *sz_alloc(ISzAllocPtr p, size_t size) { (void)p; return size ? malloc(size) : NULL; }
static void sz_free(ISzAllocPtr p, void *a) { (void)p; free(a); }
static const ISzAlloc sz_allocator = { sz_alloc, sz_free };

typedef struct { ISeekInStream vt; FILE *f; } FileSeek;

static SRes fs_read(ISeekInStreamPtr pp, void *buf, size_t *size) {
    FileSeek *p = Z7_CONTAINER_FROM_VTBL(pp, FileSeek, vt);
    size_t want = *size;
    *size = want ? fread(buf, 1, want, p->f) : 0;
    return (*size == 0 && want && ferror(p->f)) ? SZ_ERROR_READ : SZ_OK;
}

static SRes fs_seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin) {
    FileSeek *p = Z7_CONTAINER_FROM_VTBL(pp, FileSeek, vt);
    int how = origin == SZ_SEEK_SET ? SEEK_SET : origin == SZ_SEEK_CUR ? SEEK_CUR : SEEK_END;
    if (fseek(p->f, (long)*pos, how) != 0) return SZ_ERROR_READ;
    *pos = ftell(p->f);
    return SZ_OK;
}

/* Returns 1 on success, 0 on a real failure (err set), -1 if this archive
 * can't be handled here (then the .tar.xz is used instead). */
static int extract_7z(const char *archive, const char *out_path, UnzipProgress *up, volatile int *cancel,
                      char *err, size_t errlen) {
    FileSeek fsk;
    fsk.vt.Read = fs_read;
    fsk.vt.Seek = fs_seek;
    fsk.f = fopen(archive, "rb");
    if (!fsk.f) { snprintf(err, errlen, "couldn't open the download"); return 0; }
    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    look.buf = malloc(1 << 15);
    look.bufSize = 1 << 15;
    look.realStream = &fsk.vt;
    LookToRead2_INIT(&look)
    CrcGenerateTable();
    CSzArEx db;
    SzArEx_Init(&db);
    int result = -1;
    Byte *window = NULL;
    CLzmaDec dec;
    LzmaDec_CONSTRUCT(&dec)
    bool probs = false;
    unsigned char *in = NULL;
    if (!look.buf || SzArEx_Open(&db, &look.vt, &sz_allocator, &sz_allocator) != SZ_OK) goto done;
    /* find the .dol */
    int idx = -1;
    for (UInt32 i = 0; i < db.NumFiles && idx < 0; i++) {
        if (SzArEx_IsDir(&db, i)) continue;
        UInt16 name16[300];
        size_t n = SzArEx_GetFileNameUtf16(&db, i, NULL);
        if (n == 0 || n > 300) continue;
        SzArEx_GetFileNameUtf16(&db, i, name16);
        char name[300];
        for (size_t k = 0; k < n; k++) name[k] = (char)(name16[k] < 128 ? name16[k] : '?');
        name[n - 1] = 0;
        if (wanted_entry(name)) idx = (int)i;
    }
    if (idx < 0) { snprintf(err, errlen, "the Swiss .dol wasn't in the download"); result = 0; goto done; }
    UInt32 fo = db.FileToFolder[idx];
    if (fo == (UInt32)-1) goto done;
    /* only a plain LZMA block is handled here */
    CSzFolder folder;
    CSzData sd;
    sd.Data = db.db.CodersData + db.db.FoCodersOffsets[fo];
    sd.Size = db.db.FoCodersOffsets[fo + 1] - db.db.FoCodersOffsets[fo];
    if (SzGetNextFolderItem(&folder, &sd) != SZ_OK || folder.NumCoders != 1 || folder.Coders[0].MethodID != 0x030101 ||
        folder.Coders[0].PropsSize != 5)
        goto done;
    const Byte *props = db.db.CodersData + db.db.FoCodersOffsets[fo] + folder.Coders[0].PropsOffset;
    UInt64 off = db.UnpackPositions[idx] - db.UnpackPositions[db.FolderToFile[fo]];
    UInt64 size = SzArEx_GetFileSize(&db, idx);
    UInt64 need = off + size;
    if (need > SZ_WINDOW_MAX) goto done;   /* would need too much memory */
    window = malloc((size_t)need);
    in = malloc(32768);
    if (!window || !in || LzmaDec_AllocateProbs(&dec, props, 5, &sz_allocator) != SZ_OK) goto done;
    probs = true;
    dec.dic = window;
    dec.dicBufSize = (SizeT)need;
    LzmaDec_Init(&dec);
    UInt32 pack_idx = db.db.FoStartPackStreamIndex[fo];
    UInt64 pack_pos = db.dataPos + db.db.PackPositions[pack_idx];
    UInt64 pack_left = db.db.PackPositions[pack_idx + 1] - db.db.PackPositions[pack_idx];
    if (fseek(fsk.f, (long)pack_pos, SEEK_SET) != 0) { snprintf(err, errlen, "couldn't read the download"); result = 0; goto done; }
    up->total = need;
    up->done = 0;
    size_t in_len = 0, in_pos = 0;
    while (dec.dicPos < need) {
        if (*cancel) { snprintf(err, errlen, "stopped"); result = 0; goto done; }
        if (in_pos == in_len) {
            size_t want = pack_left < 32768 ? (size_t)pack_left : 32768;
            in_len = want ? fread(in, 1, want, fsk.f) : 0;
            in_pos = 0;
            pack_left -= in_len;
            if (in_len == 0) { snprintf(err, errlen, "the download ended early"); result = 0; goto done; }
        }
        SizeT in_processed = in_len - in_pos;
        ELzmaStatus status;
        SRes r = LzmaDec_DecodeToDic(&dec, (SizeT)need, in + in_pos, &in_processed, LZMA_FINISH_ANY, &status);
        in_pos += in_processed;
        up->done = dec.dicPos;
        if (r != SZ_OK) { snprintf(err, errlen, "the download is damaged"); result = 0; goto done; }
        if (status == LZMA_STATUS_FINISHED_WITH_MARK && dec.dicPos < need) { snprintf(err, errlen, "the download is damaged"); result = 0; goto done; }
    }
    if (SzBitWithVals_Check(&db.CRCs, idx) && CrcCalc(window + off, (size_t)size) != db.CRCs.Vals[idx]) {
        snprintf(err, errlen, "the download is damaged (checksum)");
        result = 0;
        goto done;
    }
    FILE *o = fopen(out_path, "wb");
    if (!o) { snprintf(err, errlen, "couldn't write to the SD card"); result = 0; goto done; }
    bool wok = fwrite(window + off, 1, (size_t)size, o) == (size_t)size;
    if (fclose(o) != 0) wok = false;
    if (!wok) { remove(out_path); snprintf(err, errlen, "couldn't write to the SD card"); result = 0; goto done; }
    result = 1;
done:
    if (probs) LzmaDec_FreeProbs(&dec, &sz_allocator);
    free(in);
    free(window);
    SzArEx_Free(&db, &sz_allocator);
    free(look.buf);
    fclose(fsk.f);
    return result;
}

static void install_failed(const char *msg) {
    snprintf(err_msg, sizeof(err_msg), "%s", msg);
    state = UPD_FAILED;
}

void swiss_worker_install(volatile int *cancel) {
    char archive[400], tmpdol[400], err[200] = "";
    if (!target[0] && !swiss_find_dol(target, sizeof(target))) target[0] = 0;   /* none yet: a fresh install */
    swiss_destination(new_path, sizeof(new_path));
    snprintf(tmpdol, sizeof(tmpdol), "%s.new", new_path);
    DownloadProgress *dp = (DownloadProgress *)fetch_download_progress();
    UnzipProgress *up = (UnzipProgress *)fetch_unzip_progress();
    /* the .7z first (smaller); the .tar.xz if the .7z can't be unpacked here */
    for (int pass = 0; pass < 2; pass++) {
        const char *url = pass == 0 ? url_7z : url_txz;
        if (!url[0]) continue;
        storage_path(archive, sizeof(archive), pass == 0 ? "swiss_update.7z" : "swiss_update.tar.xz");
        /* 1. download */
        step = UPD_STEP_DOWNLOAD;
        char part[420];
        snprintf(part, sizeof(part), "%s.part", archive);
        remove(part);
        remove(archive);
        memset(dp, 0, sizeof(*dp));
        dp->total = -1;
        err[0] = 0;
        if (!download_to_file(url, archive, dp, cancel, err, sizeof(err))) {
            remove(part);
            char m[300];
            if (*cancel) snprintf(m, sizeof(m), "Stopped. Nothing was changed.");
            else snprintf(m, sizeof(m), "%s Nothing was changed.", err);
            install_failed(m);
            return;
        }
        /* 2. unpack just the .dol */
        step = UPD_STEP_UNPACK;
        remove(tmpdol);
        err[0] = 0;
        int got = pass == 0 ? extract_7z(archive, tmpdol, up, cancel, err, sizeof(err))
                            : (extract_swiss_dol(archive, tmpdol, up, cancel, err, sizeof(err)) ? 1 : 0);
        remove(archive);
        if (got == 1) break;
        if (got < 0 && pass == 0 && url_txz[0]) continue;   /* try the .tar.xz */
        char m[300];
        snprintf(m, sizeof(m), "The download couldn't be unpacked (%s). Nothing was changed.",
                 err[0] ? err : "this kind of archive isn't supported");
        install_failed(m);
        return;
    }
    if (!is_file(tmpdol)) {
        install_failed("The download couldn't be unpacked. Nothing was changed.");
        return;
    }
    if (!dol_check(tmpdol, err, sizeof(err))) {
        remove(tmpdol);
        char m[300];
        snprintf(m, sizeof(m), "The new Swiss looks damaged (%s). Nothing was changed.", err);
        install_failed(m);
        return;
    }
    /* 3. replace: whatever is at the destination is kept as <name>.old */
    step = UPD_STEP_REPLACE;
    char bak[320];
    snprintf(bak, sizeof(bak), "%s.old", new_path);
    bool had = is_file(new_path);
    if (had) {
        remove(bak);
        if (rename(new_path, bak) != 0) {
            remove(tmpdol);
            install_failed("Couldn't replace the old Swiss. Nothing was changed.");
            return;
        }
    }
    if (rename(tmpdol, new_path) != 0) {
        if (had) rename(bak, new_path);   /* put the old one back */
        remove(tmpdol);
        install_failed("Couldn't put the new Swiss in place. The old one was kept.");
        return;
    }
    /* a swiss_rNNNN.dol that the new number replaced is set aside too */
    if (target[0] && strcmp(target, new_path) && (g_set.swiss_name == SWISS_NAME_KEEP || g_set.swiss_name == SWISS_NAME_NUMBERED)) {
        const char *sl = strrchr(target, '/');
        const char *b = sl ? sl + 1 : target;
        if (!strncasecmp(b, "swiss_r", 7) && rev_in(b) > 0) {
            char told[320];
            snprintf(told, sizeof(told), "%s.old", target);
            remove(told);
            rename(target, told);
        }
    }
    diag_note(DIAG_WORKER, "swiss: installed %s as %s", latest, new_path);
    just_installed = true;
    state = UPD_INSTALLED;
}

/* ------------------------------------------------------------ UI thread */

void swiss_request_check(void) {
    if (state == UPD_CHECKING || state == UPD_INSTALLING || state == UPD_INSTALLED) return;
    want_check = true;
    err_msg[0] = 0;
    state = UPD_CHECKING;
}

void swiss_request_install(void) {
    if (state != UPD_AVAILABLE && state != UPD_FAILED) return;
    if (!storage_ok()) { snprintf(err_msg, sizeof(err_msg), "No SD card was found."); state = UPD_FAILED; return; }
    want_install = true;
    err_msg[0] = 0;
    step = UPD_STEP_DOWNLOAD;
    state = UPD_INSTALLING;
}

void swiss_poll(void) {
    if (fetch_take_swiss()) {
        if (just_installed) {
            /* remember what's there now, and point the Exit button at it */
            just_installed = false;
            snprintf(g_set.swiss_dol, sizeof(g_set.swiss_dol), "%s", new_path);
            snprintf(g_set.swiss_rev_path, sizeof(g_set.swiss_rev_path), "%s", new_path);
            g_set.swiss_rev = latest_rev;
            snprintf(installed, sizeof(installed), "r%d", latest_rev);
            settings_save();
            char m[96];
            snprintf(m, sizeof(m), "Swiss %s is installed", latest);
            ui_toast(m);
        }
        return;
    }
    if ((want_check || want_install) && fetch_state() == FETCH_IDLE && fetch_net_ready()) {
        if (want_install) { if (fetch_start_swiss(true)) want_install = false; }
        else if (fetch_start_swiss(false)) want_check = false;
    }
}

bool swiss_launch(char *err, size_t errlen) {
    const char *p = state == UPD_INSTALLED ? new_path : g_set.swiss_dol;
    return dol_launch(p, err, errlen);
}
