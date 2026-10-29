/* sevenz.c - see sevenz.h. Uses the 7-Zip SDK by Igor Pavlov (public domain). */
#include "Precomp.h"
#include "sevenz.h"
#include "7z.h"
#include "7zCrc.h"
#include "memstat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* memory kept free for web pages and the rest of the browser while unpacking */
#define KEEP_FREE (3u * 1024u * 1024u)

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

/* The file's name in the archive, as plain text (non-ASCII becomes '_'). */
static bool entry_name(const CSzArEx *db, UInt32 i, char *out, size_t cap) {
    size_t n = SzArEx_GetFileNameUtf16(db, i, NULL);
    if (n == 0 || n > 512) return false;
    UInt16 name16[512];
    SzArEx_GetFileNameUtf16(db, i, name16);
    size_t k = 0;
    for (; k + 1 < n && k + 1 < cap; k++) out[k] = (char)(name16[k] && name16[k] < 128 ? name16[k] : '_');
    out[k] = 0;
    return k > 0;
}

bool sevenz_file(const char *archive_path, const char *dest_dir, UnzipProgress *pg, volatile int *cancel,
                 char *err, size_t errlen) {
    FileSeek fsk;
    fsk.vt.Read = fs_read;
    fsk.vt.Seek = fs_seek;
    fsk.f = fopen(archive_path, "rb");
    if (!fsk.f) { snprintf(err, errlen, "Couldn't open the 7z file"); return false; }
    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    look.buf = malloc(1 << 15);
    look.bufSize = 1 << 15;
    look.realStream = &fsk.vt;
    LookToRead2_INIT(&look)
    CrcGenerateTable();
    CSzArEx db;
    SzArEx_Init(&db);
    bool ok = false;
    UInt32 block = 0xFFFFFFFF;
    Byte *out_buf = NULL;
    size_t out_size = 0;
    if (!look.buf) { snprintf(err, errlen, "Out of memory"); goto done; }
    SRes r = SzArEx_Open(&db, &look.vt, &sz_allocator, &sz_allocator);
    if (r != SZ_OK) {
        snprintf(err, errlen, r == SZ_ERROR_MEM ? "Out of memory reading the 7z file's list of files" : "This isn't a 7z file, or it's incomplete");
        goto done;
    }
    if (!make_dirs(dest_dir)) { snprintf(err, errlen, "Couldn't make the folder for the files"); goto done; }

    /* totals, and a check that every block fits in memory before anything is written */
    pg->total = 0;
    pg->done = 0;
    pg->files_total = 0;
    pg->files_done = 0;
    for (UInt32 i = 0; i < db.NumFiles; i++)
        if (!SzArEx_IsDir(&db, i)) { pg->files_total++; pg->total += SzArEx_GetFileSize(&db, i); }
    size_t room = cb_mem_free();
    room = room > KEEP_FREE ? room - KEEP_FREE : 0;
    for (UInt32 f = 0; f < db.db.NumFolders; f++) {
        UInt64 need = SzAr_GetFolderUnpackSize(&db.db, f);
        if (need > room) {
            snprintf(err, errlen, "This 7z was packed in %u MB blocks, too big for the GameCube's memory (%u MB free)",
                     (unsigned)((need + 1048575) / 1048576), (unsigned)(room / 1048576));
            goto done;
        }
    }

    for (UInt32 i = 0; i < db.NumFiles; i++) {
        if (*cancel) { snprintf(err, errlen, "Stopped."); goto done; }
        char raw[300], name[300], path[700];
        if (!entry_name(&db, i, raw, sizeof(raw)) || !clean_name(raw, name, sizeof(name))) continue;
        snprintf(path, sizeof(path), "%s/%s", dest_dir, name);
        if (SzArEx_IsDir(&db, i)) { make_dirs(path); continue; }
        snprintf(pg->current, sizeof(pg->current), "%s", name);
        size_t offset = 0, got = 0;
        r = SzArEx_Extract(&db, &look.vt, i, &block, &out_buf, &out_size, &offset, &got, &sz_allocator, &sz_allocator);
        if (r != SZ_OK) {
            snprintf(err, errlen, r == SZ_ERROR_UNSUPPORTED ? "This 7z uses a packing method the GameCube can't unpack (only LZMA and LZMA2)"
                                  : r == SZ_ERROR_MEM ? "Out of memory" : r == SZ_ERROR_CRC ? "The 7z file is damaged (checksum)"
                                  : "The 7z file is damaged");
            goto done;
        }
        /* the file's folder, then the file */
        char dir[700];
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; make_dirs(dir); }
        FILE *o = fopen(path, "wb");
        if (!o) { snprintf(err, errlen, "Couldn't write %s to the SD card", name); goto done; }
        bool wok = got == 0 || fwrite(out_buf + offset, 1, got, o) == got;
        if (fclose(o) != 0) wok = false;
        if (!wok) { remove(path); snprintf(err, errlen, "Couldn't write to the SD card (is it full?)"); goto done; }
        pg->done += got;
        pg->files_done++;
    }
    ok = true;
done:
    free(out_buf);
    SzArEx_Free(&db, &sz_allocator);
    free(look.buf);
    fclose(fsk.f);
    return ok;
}
