/* unzip.c - zip archive extraction. */
#include "unzip.h"
#include "inflate.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_CD_BYTES (8 * 1024 * 1024)

static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }
static uint64_t rd64(const unsigned char *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

bool make_dirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t n = strlen(tmp);
    while (n && tmp[n - 1] == '/') tmp[--n] = 0;
    for (char *p = tmp; *p; p++) {
        if (*p != '/' || p == tmp || p[-1] == ':') continue;
        *p = 0;
        mkdir(tmp, 0777);
        *p = '/';
    }
    if (mkdir(tmp, 0777) == 0 || errno == EEXIST) return true;
    struct stat st;
    return stat(tmp, &st) == 0;
}

/* Makes an archive path safe for a FAT card: no absolute paths, no "..",
 * no characters FAT doesn't allow. Returns false if nothing usable is left. */
static bool clean_name(const char *in, char *out, size_t cap) {
    size_t o = 0;
    const char *p = in;
    while (*p == '/' || *p == '\\') p++;
    while (*p && o + 1 < cap) {
        const char *seg = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t len = (size_t)(p - seg);
        if (*p) p++;
        if (len == 0 || (len == 1 && seg[0] == '.')) continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') continue;
        if (o && o + 1 < cap) out[o++] = '/';
        for (size_t i = 0; i < len && o + 1 < cap; i++) {
            char c = seg[i];
            if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32)
                c = '_';
            out[o++] = c;
        }
    }
    out[o] = 0;
    return o > 0;
}

typedef struct {
    FILE *in, *out;
    uint64_t left;            /* compressed bytes still to read */
    UnzipProgress *pg;
    volatile int *cancel;
} Job;

static int job_read(void *u, unsigned char *buf, int max) {
    Job *j = u;
    if (!j->left) return 0;
    size_t want = (uint64_t)max < j->left ? (size_t)max : (size_t)j->left;
    size_t got = fread(buf, 1, want, j->in);
    j->left -= got;
    return (int)got;
}

static int job_write(void *u, const unsigned char *d, size_t n) {
    Job *j = u;
    if (j->cancel && *j->cancel) return -1;
    if (fwrite(d, 1, n, j->out) != n) return -1;
    if (j->pg) j->pg->done += n;
    return 0;
}

static bool find_eocd(FILE *f, long size, uint64_t *cd_off, uint64_t *cd_size, uint64_t *entries, char *err, size_t errlen) {
    long tail = size < 65557 ? size : 65557;
    unsigned char *buf = malloc((size_t)tail);
    if (!buf) { snprintf(err, errlen, "Out of memory"); return false; }
    fseek(f, size - tail, SEEK_SET);
    if (fread(buf, 1, (size_t)tail, f) != (size_t)tail) { free(buf); snprintf(err, errlen, "Couldn't read the zip file"); return false; }
    long pos = -1;
    for (long i = tail - 22; i >= 0; i--)
        if (rd32(buf + i) == 0x06054b50) { pos = i; break; }
    if (pos < 0) { free(buf); snprintf(err, errlen, "This isn't a zip file, or it's incomplete"); return false; }
    unsigned char *e = buf + pos;
    *entries = rd16(e + 10);
    *cd_size = rd32(e + 12);
    *cd_off = rd32(e + 16);
    /* ZIP64: look for the locator just before the end record */
    if ((*entries == 0xFFFF || *cd_size == 0xFFFFFFFF || *cd_off == 0xFFFFFFFF) && pos >= 20 &&
        rd32(e - 20) == 0x07064b50) {
        uint64_t z64 = rd64(e - 20 + 8);
        unsigned char r[56];
        if (z64 < (uint64_t)size && fseek(f, (long)z64, SEEK_SET) == 0 && fread(r, 1, 56, f) == 56 &&
            rd32(r) == 0x06064b50) {
            *entries = rd64(r + 32);
            *cd_size = rd64(r + 40);
            *cd_off = rd64(r + 48);
        }
    }
    free(buf);
    if (*cd_off + *cd_size > (uint64_t)size) { snprintf(err, errlen, "The zip file is damaged"); return false; }
    return true;
}

bool unzip_file(const char *zip_path, const char *dest_dir, UnzipProgress *pg, volatile int *cancel,
                char *err, size_t errlen) {
    err[0] = 0;
    FILE *f = fopen(zip_path, "rb");
    if (!f) { snprintf(err, errlen, "Couldn't open the zip file"); return false; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 22) { fclose(f); snprintf(err, errlen, "This isn't a zip file"); return false; }
    uint64_t cd_off, cd_size, entries;
    if (!find_eocd(f, size, &cd_off, &cd_size, &entries, err, errlen)) { fclose(f); return false; }
    if (cd_size > MAX_CD_BYTES) { fclose(f); snprintf(err, errlen, "This zip has too many files"); return false; }
    unsigned char *cd = malloc((size_t)cd_size + 1);
    if (!cd) { fclose(f); snprintf(err, errlen, "Out of memory"); return false; }
    fseek(f, (long)cd_off, SEEK_SET);
    if (fread(cd, 1, (size_t)cd_size, f) != (size_t)cd_size) {
        free(cd); fclose(f); snprintf(err, errlen, "Couldn't read the zip file"); return false;
    }

    /* first pass: totals for the progress bar */
    if (pg) { pg->done = 0; pg->total = 0; pg->files_done = 0; pg->files_total = 0; }
    size_t p = 0;
    for (uint64_t i = 0; i < entries && p + 46 <= cd_size; i++) {
        unsigned char *h = cd + p;
        if (rd32(h) != 0x02014b50) break;
        uint64_t usize = rd32(h + 24);
        size_t nl = rd16(h + 28), xl = rd16(h + 30), cl = rd16(h + 32);
        if (usize == 0xFFFFFFFF && p + 46 + nl + xl <= cd_size) {
            for (size_t x = 0; x + 4 <= xl;) {
                unsigned char *ex = h + 46 + nl + x;
                size_t sz = rd16(ex + 2);
                if (rd16(ex) == 0x0001 && sz >= 8) usize = rd64(ex + 4);
                x += 4 + sz;
            }
        }
        if (pg) { pg->total += usize; if (nl && h[46 + nl - 1] != '/') pg->files_total++; }
        p += 46 + nl + xl + cl;
    }

    bool ok = true;
    make_dirs(dest_dir);
    p = 0;
    char *buf = malloc(64 * 1024);
    for (uint64_t i = 0; i < entries && ok; i++) {
        if (p + 46 > cd_size) { snprintf(err, errlen, "The zip file is damaged"); ok = false; break; }
        unsigned char *h = cd + p;
        if (rd32(h) != 0x02014b50) { snprintf(err, errlen, "The zip file is damaged"); ok = false; break; }
        uint32_t flags = rd16(h + 8), method = rd16(h + 10), crc = rd32(h + 16);
        uint64_t csize = rd32(h + 20), usize = rd32(h + 24);
        size_t nl = rd16(h + 28), xl = rd16(h + 30), cl = rd16(h + 32);
        uint64_t lho = rd32(h + 42);
        if (p + 46 + nl + xl + cl > cd_size) { snprintf(err, errlen, "The zip file is damaged"); ok = false; break; }
        /* ZIP64 sizes/offset live in an extra field */
        for (size_t x = 0; x + 4 <= xl;) {
            unsigned char *ex = h + 46 + nl + x;
            size_t sz = rd16(ex + 2), q = 4;
            if (rd16(ex) == 0x0001) {
                if (usize == 0xFFFFFFFF && q + 8 <= 4 + sz) { usize = rd64(ex + q); q += 8; }
                if (csize == 0xFFFFFFFF && q + 8 <= 4 + sz) { csize = rd64(ex + q); q += 8; }
                if (lho == 0xFFFFFFFF && q + 8 <= 4 + sz) { lho = rd64(ex + q); q += 8; }
            }
            x += 4 + sz;
        }
        char raw[300], name[300], path[600];
        size_t cn = nl < sizeof(raw) - 1 ? nl : sizeof(raw) - 1;
        memcpy(raw, h + 46, cn);
        raw[cn] = 0;
        p += 46 + nl + xl + cl;
        bool is_dir = cn && (raw[cn - 1] == '/' || raw[cn - 1] == '\\');
        if (!clean_name(raw, name, sizeof(name))) continue;
        snprintf(path, sizeof(path), "%s/%s", dest_dir, name);
        if (is_dir) { make_dirs(path); continue; }
        if (pg) snprintf(pg->current, sizeof(pg->current), "%s", name);
        if (flags & 1) { snprintf(err, errlen, "%s is password-protected", name); ok = false; break; }
        if (method != 0 && method != 8) { snprintf(err, errlen, "%s uses a compression type that isn't supported", name); ok = false; break; }
        if (lho > 0x7FFFFFFF || csize > 0xFFFFFFFFull || usize > 0xFFFFFFFFull) {
            snprintf(err, errlen, "%s is too large for this SD card format", name); ok = false; break;
        }
        /* the local header's own name/extra lengths may differ from the central copy */
        unsigned char lh[30];
        if (fseek(f, (long)lho, SEEK_SET) != 0 || fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) {
            snprintf(err, errlen, "The zip file is damaged"); ok = false; break;
        }
        fseek(f, (long)(lho + 30 + rd16(lh + 26) + rd16(lh + 28)), SEEK_SET);
        /* make the folder for this file */
        char *slash = strrchr(path, '/');
        if (slash) { *slash = 0; make_dirs(path); *slash = '/'; }
        FILE *out = fopen(path, "wb");
        if (!out) { snprintf(err, errlen, "Couldn't create %s (is the SD card full?)", name); ok = false; break; }
        if (buf) setvbuf(out, buf, _IOFBF, 64 * 1024);
        Job j = { f, out, csize, pg, cancel };
        uint64_t got = 0;
        uint32_t gotcrc = 0;
        int r = 0;
        if (method == 8) {
            r = inflate_stream(job_read, &j, job_write, &j, &got, &gotcrc);
        } else {
            unsigned char tmp[16384];
            int n;
            while ((n = job_read(&j, tmp, sizeof(tmp))) > 0) {
                gotcrc = crc32_update(gotcrc, tmp, (size_t)n);
                got += (uint64_t)n;
                if (job_write(&j, tmp, (size_t)n) < 0) { r = -3; break; }
            }
        }
        bool wrote = fclose(out) == 0;
        if (cancel && *cancel) { remove(path); snprintf(err, errlen, "Stopped."); ok = false; break; }
        if (r == -3 || !wrote) { remove(path); snprintf(err, errlen, "Couldn't write %s (is the SD card full?)", name); ok = false; break; }
        if (r < 0 || got != usize || gotcrc != crc) {
            remove(path); snprintf(err, errlen, "%s is damaged inside the zip", name); ok = false; break;
        }
        if (pg) pg->files_done++;
    }
    free(buf);
    free(cd);
    fclose(f);
    return ok;
}
