/* browser.c - toolbar, page view, status bar, history and bookmarks. */
#include "browser.h"
#include "page.h"
#include "fetch.h"
#include "netio.h"
#include "ui.h"
#include "osk.h"
#include "theme.h"
#include "input.h"
#include "settings.h"
#include "storage.h"
#include "home.h"
#include "sound.h"
#include "url.h"
#include "image.h"
#include "diag.h"
#include "unzip.h"
#include "dolload.h"
#include "cookies.h"
#include <sys/stat.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define HIST_MAX 32
#define BOOKMARK_MAX 64

/* A history entry remembers how to get the page back: its address, any form
 * data it was submitted with, and (for recent pages) the page itself. */
typedef struct {
    char url[URL_MAX];
    float scroll;
    char *post;          /* form data for pages that came from a POST, or NULL */
    char *src;           /* cached page source, or NULL */
    size_t len;
    bool plain, secure, verified;
} HistEntry;

#define HIST_CACHE_PAGES 4
#define HIST_CACHE_BYTES (2u * 1024u * 1024u)
typedef struct { char title[96]; char url[URL_MAX]; } Bookmark;

static Page page;
static bool has_page;
static int page_gen;
static HistEntry back_stack[HIST_MAX], fwd_stack[HIST_MAX];
static int nback, nfwd;
static bool loading;
static char loading_url[URL_MAX];
static float restore_scroll = -1;
static bool settings_request;
static Bookmark bookmarks[BOOKMARK_MAX];
static int nbookmarks;
static bool bm_open;
static int bm_scroll;
static int confirm_delete = -1;
static float last_layout_w, last_layout_scale;
static bool queued;
static char queued_url[URL_MAX];
static char *queued_post;

/* The start page is drawn by home.c; this only gives it a title (and is
 * what shows if something ever asks the page engine to draw it). */
static const char home_html[] =
    "<title>Start</title><h1>CubeBrowser</h1><p>Press <b>X</b> to type an address or search.</p>";

static const char help_html[] =
    "<title>Controls</title><h1>Controls</h1><ul>"
    "<li><b>Control Stick</b> - move the pointer (hold Z for fine control)</li>"
    "<li><b>A</b> - click</li><li><b>B</b> - back</li>"
    "<li><b>X</b> - type an address or search</li><li><b>Y</b> - bookmarks</li>"
    "<li><b>START</b> - settings</li><li><b>C-Stick</b> - scroll</li>"
    "<li><b>L / R</b> - page up / page down</li>"
    "<li><b>D-Pad</b> - jump between links and buttons</li>"
    "<li><b>Hold Z + A</b> on a link - save it to the SD card instead of opening it</li></ul>"

    "<p>All buttons except the D-Pad can be changed in Settings &gt; Controller.</p>"
    "<h3>On-screen keyboard</h3><ul><li><b>B</b> delete, <b>Y</b> space, <b>X</b> shift</li>"
    "<li><b>L / R</b> move the text cursor, <b>START</b> accept</li></ul>"
    "<h3>Downloads</h3><p>Links to files (such as .dol, .iso or .zip) ask to save them to the SD card. "
    "Zip files can be unzipped automatically. Interrupted downloads continue where they stopped.</p>"
    "<h3>What works</h3><p>Text, links, pictures, tables, lists, forms such as search boxes, and most "
    "page colours, fonts and spacing. JavaScript and modern multi-column layouts aren't supported, so "
    "sites are shown in a simpler, phone-style layout. Lightweight sites work best.</p>"
    "<p><a href=\"about:home\">Back to the start page</a></p>";

/* ------------------------------------------------------------ bookmarks */

static void bookmarks_load(void) {
    nbookmarks = 0;
    size_t len;
    char file[400];
    storage_path(file, sizeof(file), "bookmarks.txt");
    char *data = storage_read(file, &len);
    if (!data) return;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line && nbookmarks < BOOKMARK_MAX; line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t');
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        if (tab) *tab = 0;
        if (!line[0]) continue;
        Bookmark *b = &bookmarks[nbookmarks++];
        snprintf(b->url, sizeof(b->url), "%s", line);
        snprintf(b->title, sizeof(b->title), "%s", tab ? tab + 1 : line);
    }
    free(data);
}

static void bookmarks_save(void) {
    size_t cap = (size_t)nbookmarks * (URL_MAX + 100) + 16, n = 0;
    char *buf = malloc(cap);
    if (!buf) return;
    for (int i = 0; i < nbookmarks; i++)
        n += snprintf(buf + n, cap - n, "%s\t%s\n", bookmarks[i].url, bookmarks[i].title);
    char file[400], tmp[400];
    storage_path(file, sizeof(file), "bookmarks.txt");
    storage_path(tmp, sizeof(tmp), "bookmarks.tmp");
    if (!storage_write_atomic(file, tmp, buf, n))
        ui_toast("Couldn't save bookmarks to the SD card");
    free(buf);
}

void browser_clear_bookmarks(void) {
    nbookmarks = 0;
    bookmarks_save();
}

static void bookmark_add_current(void) {
    if (!has_page) return;
    for (int i = 0; i < nbookmarks; i++)
        if (!strcmp(bookmarks[i].url, page.url)) { ui_toast("Already bookmarked"); return; }
    if (nbookmarks >= BOOKMARK_MAX) { ui_toast("Bookmark list is full"); return; }
    Bookmark *b = &bookmarks[nbookmarks++];
    snprintf(b->url, sizeof(b->url), "%s", page.url);
    snprintf(b->title, sizeof(b->title), "%s", page.title[0] ? page.title : page.url);
    for (char *c = b->title; *c; c++) if (*c == '\t') *c = ' ';
    bookmarks_save();
    ui_toast("Bookmark added");
}

/* ---------------------------------------------------------- navigation */

static void html_escape(const char *in, char *out, size_t outlen) {
    size_t n = 0;
    for (; *in && n + 7 < outlen; in++) {
        const char *rep = NULL;
        if (*in == '<') rep = "&lt;";
        else if (*in == '>') rep = "&gt;";
        else if (*in == '&') rep = "&amp;";
        else if (*in == '"') rep = "&quot;";
        if (rep) { size_t l = strlen(rep); memcpy(out + n, rep, l); n += l; }
        else out[n++] = *in;
    }
    out[n] = 0;
}

static void show_html(const char *html, const char *url, bool plain) {
    char *copy = strdup(html);
    if (!copy) return;
    page_set_source(&page, copy, strlen(copy), plain, url);
    page.secure = page.verified = page.truncated = false;
    has_page = true;
    page_gen++;
    last_layout_w = 0;
}

static void show_error(const char *url, const char *msg) {
    static char eu[URL_MAX * 6], em[1600], html[16384];
    html_escape(url, eu, sizeof(eu));  /* worst case: every char escaped */
    html_escape(msg, em, sizeof(em));
    snprintf(html, sizeof(html),
             "<title>Problem loading page</title><h2>This page couldn't be opened</h2><p>%s</p>"
             "<p>Address: %s</p><p><a href=\"%s\">Try again</a> &nbsp; <a href=\"about:home\">Start page</a></p>",
             em, eu, eu);
    show_html(html, url, false);
    sound_play(SFX_ERROR);
}

static char *cur_post;       /* form data the current page was loaded with */
static char *pending_post;   /* form data of the page being loaded */

static void hist_free(HistEntry *e) {
    free(e->post);
    free(e->src);
    e->post = e->src = NULL;
}

/* Keep only the most recent few pages in memory. */
static void hist_trim_cache(void) {
    size_t bytes = 0;
    int count = 0;
    /* newest first: forward stack top, then back stack top */
    HistEntry *order[HIST_MAX * 2];
    int n = 0;
    for (int i = nback - 1; i >= 0; i--) order[n++] = &back_stack[i];
    for (int i = nfwd - 1; i >= 0; i--) order[n++] = &fwd_stack[i];
    for (int i = 0; i < n; i++) {
        HistEntry *e = order[i];
        if (!e->src) continue;
        if (count >= HIST_CACHE_PAGES || bytes + e->len > HIST_CACHE_BYTES) { free(e->src); e->src = NULL; continue; }
        count++;
        bytes += e->len;
    }
}

/* Saves the current page into an entry. The page's source moves into the
 * entry (the page on screen no longer needs it once it has been read). */
static void hist_capture(HistEntry *e) {
    memset(e, 0, sizeof(*e));
    snprintf(e->url, URL_MAX, "%s", page.url);
    e->scroll = page.scroll_target;
    e->post = cur_post ? strdup(cur_post) : NULL;
    e->src = page.src;
    e->len = page.src_len;
    e->plain = page.plain;
    e->secure = page.secure;
    e->verified = page.verified;
    page.src = NULL;
}

static void push_history(void) {
    if (!has_page) return;
    if (nback == HIST_MAX) {
        hist_free(&back_stack[0]);
        memmove(back_stack, back_stack + 1, sizeof(HistEntry) * (HIST_MAX - 1));
        nback--;
    }
    hist_capture(&back_stack[nback++]);
    hist_trim_cache();
}

static void clear_forward(void) {
    for (int i = 0; i < nfwd; i++) hist_free(&fwd_stack[i]);
    nfwd = 0;
}

static void start_load(const char *url, const char *post);

/* Shows a history entry: instantly from memory if cached, otherwise by
 * fetching it again (re-sending its form data if it had any). */
static void restore_entry(HistEntry *e) {
    if (loading) { queued = false; fetch_cancel(); loading = false; }
    free(cur_post);
    cur_post = e->post;
    e->post = NULL;
    if (e->src) {
        page_set_source(&page, e->src, e->len, e->plain, e->url);
        e->src = NULL;
        page.secure = e->secure;
        page.verified = e->verified;
        has_page = true;
        page_gen++;
        last_layout_w = 0;
        restore_scroll = e->scroll;
        return;
    }
    restore_scroll = e->scroll;
    char url[URL_MAX];
    snprintf(url, sizeof(url), "%s", e->url);
    start_load(url, cur_post);
}

static void start_load(const char *url, const char *post) {
    if (!strncasecmp(url, "about:", 6)) {
        if (loading) { queued = false; fetch_cancel(); loading = false; }
        free(cur_post);
        cur_post = NULL;
        if (!strcasecmp(url, "about:home") || !strcasecmp(url, "about:blank") || !strcasecmp(url, "about:"))
        { show_html(home_html, "about:home", false); home_reset(); }
        else if (!strcasecmp(url, "about:help"))
            show_html(help_html, "about:help", false);
        else
            show_error(url, "There's no built-in page with that name.");
        if (restore_scroll >= 0) { page.scroll = page.scroll_target = restore_scroll; restore_scroll = -1; }
        return;
    }
    if (netio_state() == NET_FAILED) fetch_retry_network();
    loading = true;
    snprintf(loading_url, sizeof(loading_url), "%s", url);
    if (post != pending_post) {
        free(pending_post);
        pending_post = post ? strdup(post) : NULL;
    }
    fetch_set_referer(has_page ? page.url : NULL);
    if (fetch_state() != FETCH_IDLE) {
        /* stop the current download; this one starts as soon as it finishes */
        fetch_cancel();
        queued = true;
        snprintf(queued_url, sizeof(queued_url), "%s", url);
        free(queued_post);
        queued_post = post ? strdup(post) : NULL;
        return;
    }
    fetch_start(url, post);
}

static void navigate(const char *url, const char *post, bool record) {
    if (!url[0]) return;
    if (record) {
        push_history();
        clear_forward();
    }
    start_load(url, post);
}

void browser_navigate(const char *url) { navigate(url, NULL, true); }

/* "Save link": fetch just the headers and go straight to the save prompt. */
static void save_link(const char *url) {
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) {
        ui_toast("Only web (http/https) links can be saved");
        return;
    }
    if (fetch_state() != FETCH_IDLE) { ui_toast("Busy - try again in a moment"); return; }
    if (!fetch_start_save(url)) { ui_toast("The network isn't ready yet"); return; }
    loading = true;
    snprintf(loading_url, sizeof(loading_url), "%s", url);
}

static void go_back(void) {
    if (nback == 0) { sound_play(SFX_ERROR); return; }
    if (has_page && nfwd < HIST_MAX) hist_capture(&fwd_stack[nfwd++]);
    HistEntry e = back_stack[--nback];
    sound_play(SFX_BACK);
    restore_entry(&e);
    hist_free(&e);
    hist_trim_cache();
}

static void go_forward(void) {
    if (nfwd == 0) return;
    if (has_page) {
        if (nback == HIST_MAX) { hist_free(&back_stack[0]); memmove(back_stack, back_stack + 1, sizeof(HistEntry) * (HIST_MAX - 1)); nback--; }
        hist_capture(&back_stack[nback++]);
    }
    HistEntry e = fwd_stack[--nfwd];
    restore_entry(&e);
    hist_free(&e);
    hist_trim_cache();
}

void browser_clear_history(void) {
    for (int i = 0; i < nback; i++) hist_free(&back_stack[i]);
    nback = 0;
    clear_forward();
}


/* ------------------------------------------------------------- downloads */

enum { DL_IDLE, DL_OFFER, DL_RUNNING, DL_DONE, DL_FAILED, DL_UNZIPPING, DL_UNZIPPED, DL_UNZIP_FAILED };
static int dl_state;
static char dl_url[URL_MAX], dl_name[128], dl_path[600], dl_msg[256], dl_unzip_dir[600];
static long long dl_size;
static bool dl_is_zip, dl_unzip_after, dl_confirm_cancel;
static int dl_files;

static void fmt_size(long long b, char *out, size_t n) {
    if (b < 0) snprintf(out, n, "unknown size");
    else if (b >= 1024LL * 1024 * 1024) snprintf(out, n, "%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024) snprintf(out, n, "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024) snprintf(out, n, "%lld KB", b / 1024);
    else snprintf(out, n, "%lld bytes", b);
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcasecmp(s + a - b, suffix);
}

/* A safe file name from the server's suggestion or the address. */
static void pick_download_name(const char *url, const char *suggested) {
    char raw[256] = "";
    if (suggested && suggested[0]) {
        snprintf(raw, sizeof(raw), "%s", suggested);
    } else {
        char path[URL_MAX];
        snprintf(path, sizeof(path), "%s", url);
        char *q = strpbrk(path, "?#");
        if (q) *q = 0;
        size_t n = strlen(path);
        while (n && path[n - 1] == '/') path[--n] = 0;
        const char *slash = strrchr(path, '/');
        const char *seg = slash ? slash + 1 : path;
        size_t o = 0;   /* undo %20-style escapes */
        for (const char *c = seg; *c && o + 1 < sizeof(raw); c++) {
            if (*c == '%' && isxdigit((unsigned char)c[1]) && isxdigit((unsigned char)c[2])) {
                char hex[3] = { c[1], c[2], 0 };
                raw[o++] = (char)strtol(hex, NULL, 16);
                c += 2;
            } else {
                raw[o++] = *c;
            }
        }
        raw[o] = 0;
    }
    size_t o = 0;
    for (const char *c = raw; *c && o + 1 < 100; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch < 32 || ch >= 127 || strchr("\\/:*?\"<>|", ch)) ch = '_';
        dl_name[o++] = (char)ch;
    }
    dl_name[o] = 0;
    while (o && (dl_name[o - 1] == '.' || dl_name[o - 1] == ' ')) dl_name[--o] = 0;
    if (!dl_name[0] || dl_name[0] == '.') snprintf(dl_name, sizeof(dl_name), "download.bin");

    /* an unfinished download of the same name is continued; finished ones aren't overwritten */
    snprintf(dl_path, sizeof(dl_path), "%s/%s", g_set.dl_folder, dl_name);
    char part[620];
    snprintf(part, sizeof(part), "%s.part", dl_path);
    if (file_exists(part) || !file_exists(dl_path)) return;
    char base[128], ext[32] = "";
    snprintf(base, sizeof(base), "%s", dl_name);
    char *dot = strrchr(base, '.');
    if (dot && strlen(dot) < sizeof(ext)) { snprintf(ext, sizeof(ext), "%s", dot); *dot = 0; }
    for (int k = 2; k < 100; k++) {
        snprintf(dl_name, sizeof(dl_name), "%.90s (%d)%s", base, k, ext);
        snprintf(dl_path, sizeof(dl_path), "%s/%s", g_set.dl_folder, dl_name);
        if (!file_exists(dl_path)) break;
    }
}

static void offer_download(const HttpResult *r) {
    snprintf(dl_url, sizeof(dl_url), "%s", r->url);
    pick_download_name(r->url, r->filename);
    dl_size = r->length;
    dl_is_zip = ends_with_ci(dl_name, ".zip");
    dl_unzip_after = false;
    dl_confirm_cancel = false;
    dl_state = DL_OFFER;
    /* the link didn't lead to a new page, so don't add one to Back */
    if (nback > 0 && has_page && !strcmp(back_stack[nback - 1].url, page.url)) {
        HistEntry *e = &back_stack[--nback];
        if (e->src && !page.src) { page.src = e->src; page.src_len = e->len; e->src = NULL; }   /* hand the source back */
        hist_free(e);
    }
    sound_play(SFX_OPEN);
}

static void start_download(void) {
    if (!storage_ok()) { snprintf(dl_msg, sizeof(dl_msg), "No SD card was found."); dl_state = DL_FAILED; return; }
    make_dirs(g_set.dl_folder);
    if (!fetch_start_download(dl_url, dl_path)) {
        snprintf(dl_msg, sizeof(dl_msg), "The network is busy or not connected. Try again in a moment.");
        dl_state = DL_FAILED;
        return;
    }
    dl_confirm_cancel = false;
    dl_state = DL_RUNNING;
}

static void start_unzip(void) {
    char base[128];
    snprintf(base, sizeof(base), "%s", dl_name);
    if (ends_with_ci(base, ".zip")) base[strlen(base) - 4] = 0;
    snprintf(dl_unzip_dir, sizeof(dl_unzip_dir), "%s/%s", g_set.dl_folder, base);
    if (!fetch_start_unzip(dl_path, dl_unzip_dir)) {
        snprintf(dl_msg, sizeof(dl_msg), "Couldn't start unzipping. Try again in a moment.");
        dl_state = DL_UNZIP_FAILED;
        return;
    }
    dl_confirm_cancel = false;
    dl_state = DL_UNZIPPING;
}

static void handle_task_result(void) {
    bool ok;
    char err[256];
    if (!fetch_take_task(&ok, err, sizeof(err))) return;
    if (dl_state == DL_RUNNING) {
        if (ok) {
            sound_play(SFX_LOADED);
            if (dl_unzip_after) start_unzip();
            else dl_state = DL_DONE;
        } else if (!strcmp(err, "Stopped.")) {
            dl_state = DL_IDLE;
            ui_toast("Download cancelled");
        } else {
            snprintf(dl_msg, sizeof(dl_msg), "%s", err);
            dl_state = DL_FAILED;
            sound_play(SFX_ERROR);
        }
    } else if (dl_state == DL_UNZIPPING) {
        if (ok) {
            dl_files = fetch_unzip_progress()->files_done;
            if (g_set.dl_delete_zip) remove(dl_path);
            dl_state = DL_UNZIPPED;
            sound_play(SFX_LOADED);
        } else if (!strcmp(err, "Stopped.")) {
            dl_state = DL_DONE;
            ui_toast("Unzipping cancelled");
        } else {
            snprintf(dl_msg, sizeof(dl_msg), "%s", err);
            dl_state = DL_UNZIP_FAILED;
            sound_play(SFX_ERROR);
        }
    }
}

static void wrap_text(FontId f, float size, float x, float y, float w, Color c, const char *text, int max_lines) {
    const char *p = text;
    float lh = gfx_line_height(f, size);
    for (int line = 0; *p && line < max_lines; line++) {
        int n = gfx_text_fit(f, size, p, -1, w);
        if (n <= 0) break;
        if (p[n]) {   /* break at a space if possible */
            int k = n;
            while (k > 0 && p[k] != ' ') k--;
            if (k > n / 2) n = k;
        }
        gfx_text(f, size, x, y + line * lh, c, p, n);
        p += n;
        while (*p == ' ') p++;
    }
}

static void draw_download(bool interactive) {
    const Theme *t = theme();
    float sx, sy, sw, sh;
    ui_safe_area(&sx, &sy, &sw, &sh);
    float open = ui_anim(ui_id("dl-open", 0), 1.0f, 0.25f);
    gfx_rect(0, 0, gfx_width(), 480, color_alpha(t->shadow, 0.6f * open));
    float pw = fminf(560, sw - 20), ph = 290;
    float px = sx + (sw - pw) * 0.5f, py = sy + (sh - ph) * 0.5f + (1 - open) * 60;
    ui_set_input(interactive);
    ui_panel(px, py, pw, ph, 20);
    char a[48], b[48], line[300];
    const char *title = "";
    switch (dl_state) {
    case DL_OFFER: title = "Save to SD card?"; break;
    case DL_RUNNING: title = "Downloading"; break;
    case DL_DONE: title = "Download complete"; break;
    case DL_FAILED: title = "Download stopped"; break;
    case DL_UNZIPPING: title = "Unzipping"; break;
    case DL_UNZIPPED: title = "Unzipped"; break;
    case DL_UNZIP_FAILED: title = "Couldn't unzip"; break;
    }
    gfx_text(FONT_UI, 24, px + 24, py + 16, t->text, title, -1);
    float tx = px + 24, ty = py + 58, tw = pw - 48;
    gfx_text(FONT_UI, 18, tx, ty, t->text, dl_name, gfx_text_fit(FONT_UI, 18, dl_name, -1, tw));
    ty += 32;
    float by = py + ph - 58, bh = 40;
    bool close = false;

    if (dl_state == DL_OFFER) {
        fmt_size(dl_size, a, sizeof(a));
        snprintf(line, sizeof(line), "Size: %s", a);
        gfx_text(FONT_BODY, 17, tx, ty, t->text_dim, line, -1);
        snprintf(line, sizeof(line), "Saves to: %s", g_set.dl_folder);
        gfx_text(FONT_BODY, 17, tx, ty + 26, t->text_dim, line, gfx_text_fit(FONT_BODY, 17, line, -1, tw));
        if (dl_size > 200LL * 1024 * 1024)
            wrap_text(FONT_BODY, 16, tx, ty + 58, tw, t->text_dim, "This is a big file, so it may take a while. Keep the GameCube on until it finishes.", 2);
        int nb = dl_is_zip ? 3 : 2;
        float bw = (tw - 12 * (nb - 1)) / nb, bx = tx;
        if (ui_button(ui_id("dl-go", 0), bx, by, bw, bh, "Download", 17, UIB_ACCENT)) { dl_unzip_after = false; start_download(); }
        bx += bw + 12;
        if (dl_is_zip) {
            if (ui_button(ui_id("dl-go", 1), bx, by, bw, bh, "Download & unzip", 17, UIB_NORMAL)) { dl_unzip_after = true; start_download(); }
            bx += bw + 12;
        }
        if (ui_button(ui_id("dl-go", 2), bx, by, bw, bh, "Cancel", 17, UIB_NORMAL)) close = true;
    } else if (dl_state == DL_RUNNING || dl_state == DL_UNZIPPING) {
        long long done, total;
        if (dl_state == DL_RUNNING) {
            const DownloadProgress *pg = fetch_download_progress();
            done = pg->done; total = pg->total;
            fmt_size(done, a, sizeof(a));
            fmt_size(total, b, sizeof(b));
            char speed[48] = "";
            if (pg->speed > 0) {
                fmt_size(pg->speed, speed, sizeof(speed));
                strcat(speed, "/s");
            }
            char conns[24] = "";
            if (pg->connections > 1) snprintf(conns, sizeof(conns), "   %d connections", pg->connections);
            if (total > 0) snprintf(line, sizeof(line), "%s of %s   %s%s", a, b, speed, conns);
            else snprintf(line, sizeof(line), "%s   %s%s", a, speed, conns);
            gfx_text(FONT_BODY, 17, tx, ty, t->text, line, -1);
            if (total > 0 && pg->speed > 0) {
                long long left = (total - done) / pg->speed;
                if (left >= 3600) snprintf(line, sizeof(line), "About %lld hr %lld min left", left / 3600, (left % 3600) / 60);
                else if (left >= 60) snprintf(line, sizeof(line), "About %lld min left", left / 60 + 1);
                else snprintf(line, sizeof(line), "Less than a minute left");
                gfx_text(FONT_BODY, 17, tx, ty + 26, t->text_dim, line, -1);
            } else if (pg->resumed) {
                gfx_text(FONT_BODY, 17, tx, ty + 26, t->text_dim, "Continuing where it stopped before", -1);
            }
        } else {
            const UnzipProgress *up = fetch_unzip_progress();
            done = (long long)up->done; total = (long long)up->total;
            snprintf(line, sizeof(line), "File %d of %d: %s", up->files_done + 1 > up->files_total ? up->files_total : up->files_done + 1,
                     up->files_total, up->current);
            gfx_text(FONT_BODY, 17, tx, ty, t->text, line, gfx_text_fit(FONT_BODY, 17, line, -1, tw));
        }
        /* progress bar (an animated stripe when the size isn't known) */
        float bar_y = ty + 62;
        gfx_rrect(tx, bar_y, tw, 18, 9, color_alpha(t->button_bottom, 0.6f));
        if (total > 0) {
            float frac = (float)done / (float)total;
            if (frac > 1) frac = 1;
            gfx_rrect_vgrad(tx, bar_y, fmaxf(18, tw * frac), 18, 9, t->accent_hi, t->accent);
            snprintf(line, sizeof(line), "%d%%", (int)(frac * 100));
            gfx_text_centered(FONT_UI, 14, tx + tw * 0.5f, bar_y + 1, t->text, line);
        } else {
            float k = fmodf(ui_time() * 0.6f, 1.0f);
            gfx_rrect(tx + (tw - 80) * k, bar_y, 80, 18, 9, t->accent);
        }
        ui_spinner(px + pw - 40, py + 34, 24);
        const char *lbl = dl_confirm_cancel ? "Press again to cancel" : "Cancel";
        if (ui_button(ui_id("dl-cancel", 0), tx + tw * 0.25f, by, tw * 0.5f, bh, lbl, 17, dl_confirm_cancel ? UIB_ACCENT : UIB_NORMAL)) {
            if (dl_confirm_cancel) fetch_cancel();
            dl_confirm_cancel = !dl_confirm_cancel;
        }
    } else if (dl_state == DL_DONE) {
        snprintf(line, sizeof(line), "Saved to %s", dl_path);
        wrap_text(FONT_BODY, 17, tx, ty, tw, t->text_dim, line, 2);
        wrap_text(FONT_BODY, 16, tx, ty + 56, tw, t->text_dim, "You can open it from Swiss after leaving the browser.", 2);
        float bw = dl_is_zip ? (tw - 12) / 2 : tw * 0.5f, bx = dl_is_zip ? tx : tx + tw * 0.25f;
        if (dl_is_zip) {
            if (ui_button(ui_id("dl-done", 0), bx, by, bw, bh, "Unzip", 17, UIB_NORMAL)) start_unzip();
            bx += bw + 12;
        }
        if (ui_button(ui_id("dl-done", 1), bx, by, bw, bh, "OK", 17, UIB_ACCENT)) close = true;
    } else if (dl_state == DL_UNZIPPED) {
        snprintf(line, sizeof(line), "%d file%s extracted to %s", dl_files, dl_files == 1 ? "" : "s", dl_unzip_dir);
        wrap_text(FONT_BODY, 17, tx, ty, tw, t->text_dim, line, 3);
        if (ui_button(ui_id("dl-uz", 0), tx + tw * 0.25f, by, tw * 0.5f, bh, "OK", 17, UIB_ACCENT)) close = true;
    } else {   /* failed */
        wrap_text(FONT_BODY, 17, tx, ty, tw, t->text, dl_msg, 3);
        if (dl_state == DL_FAILED) {
            float bw = (tw - 12) / 2;
            if (ui_button(ui_id("dl-fail", 0), tx, by, bw, bh, "Retry", 17, UIB_ACCENT)) start_download();
            if (ui_button(ui_id("dl-fail", 1), tx + bw + 12, by, bw, bh, "Close", 17, UIB_NORMAL)) close = true;
        } else if (ui_button(ui_id("dl-fail", 2), tx + tw * 0.25f, by, tw * 0.5f, bh, "OK", 17, UIB_ACCENT)) {
            dl_state = DL_DONE;   /* back to the downloaded zip */
        }
    }
    if (interactive && dl_state != DL_RUNNING && dl_state != DL_UNZIPPING && input_pressed(ACT_BACK)) {
        input_consume(ACT_BACK);
        close = true;
    }
    if (close) { dl_state = DL_IDLE; sound_play(SFX_CLOSE); }
}

/* ---------------------------------------------------------------- images */

#define IMAGE_BUDGET (6u * 1024u * 1024u)   /* texture memory per page */

static void apply_image(ImageResult *ir) {
    int gen = ir->tag >> 8, idx = ir->tag & 0xFF;
    diag_note(DIAG_MAIN, "picture %d arrived: %s %dx%d", idx, ir->ok ? "ok" : ir->error, ir->img.w, ir->img.h);
    if (gen != (page_gen & 0xFFFFFF) || idx >= page.nimages) { image_free(&ir->img); return; }
    PageImage *im = &page.images[idx];
    if (!ir->ok) {
        im->state = IMG_FAILED;
        /* with a declared size its box is already in place: nothing moves */
        if (im->decl_w && im->decl_h) return;
    } else if (page.image_bytes + ir->img.bytes > IMAGE_BUDGET) {
        image_free(&ir->img);
        im->state = IMG_SKIPPED;
    } else {
        gfx_image_init(&im->img, ir->img.tex, ir->img.w, ir->img.h, ir->img.tex_w, ir->img.tex_h);
        page.image_bytes += ir->img.bytes;
        im->state = IMG_READY;
        /* a picture with a declared size already has its space: just draw it */
        if (im->decl_w && im->decl_h) return;
    }
    page.relayout = true;
}

static void image_request_url(const char *src, int max_w, int max_h, char *out, size_t n) {
    if (!g_set.image_proxy) { snprintf(out, n, "%s", src); return; }
    static char enc[URL_MAX];
    url_encode_component(src, enc, sizeof(enc));
    const Theme *t = theme();
    /* weserv fetches the picture, shrinks it and sends a small JPEG; bg fills transparency */
    int w = snprintf(out, n, "https://images.weserv.nl/?url=%s&w=%d&h=%d&fit=inside&we&output=jpg&q=70"
                     "&bg=%02x%02x%02x", enc, max_w, max_h, t->page_bg.r, t->page_bg.g, t->page_bg.b);
    if (w >= (int)n) out[0] = 0;
}

/* Starts the next image download: ones on screen first, then the rest. */
static bool sheets_pending(void) {
    if (!g_set.page_styles) return false;
    for (int i = 0; i < page.nsheets; i++)
        if (page.sheets[i].state != SHEET_DONE) return true;
    return false;
}

/* Downloads the page's stylesheets first, then its pictures. */
static void schedule_images(float view_top, float view_h, float max_w) {
    if (loading || queued || fetch_state() != FETCH_IDLE || !fetch_net_ready()) return;
    if (g_set.page_styles) {
        int si = page_next_sheet(&page);
        if (si >= 0) {
            int tag = ((page_gen & 0xFFFFFF) << 8) | si;
            if (!fetch_start_css(page.sheets[si].url, tag)) page.sheets[si].state = SHEET_NONE;
            return;
        }
        if (sheets_pending()) return;
    }
    if (!g_set.load_images) return;
    if (page.image_bytes >= IMAGE_BUDGET) {
        bool changed = false;
        for (int i = 0; i < page.nimages; i++)
            if (page.images[i].state == IMG_NONE) { page.images[i].state = IMG_SKIPPED; changed = true; }
        if (changed) page.relayout = true;
        return;
    }
    int pick = -1;
    for (int pass = 0; pass < 2 && pick < 0; pass++) {
        for (int i = 0; i < page.nboxes; i++) {
            Box *b = &page.boxes[i];
            if (b->type != BOX_IMAGE || page.images[b->text].state != IMG_NONE) continue;
            if (pass == 0 && (b->y + b->h < view_top - 100 || b->y > view_top + view_h + 400)) continue;
            pick = b->text;
            break;
        }
    }
    if (pick < 0) return;
    PageImage *im = &page.images[pick];
    static char req[URL_MAX];
    int max_h = (int)settings_max_picture_height();
    image_request_url(im->url, (int)max_w, max_h, req, sizeof(req));
    int tag = ((page_gen & 0xFFFFFF) << 8) | pick;
    if (req[0] && fetch_start_image(req, (int)max_w, max_h, tag)) im->state = IMG_LOADING;
    else if (!req[0]) { im->state = IMG_SKIPPED; page.relayout = true; }
}

static int images_pending(void) {
    int n = 0;
    for (int i = 0; i < page.nimages; i++)
        if (page.images[i].state == IMG_NONE || page.images[i].state == IMG_LOADING) n++;
    return n;
}

static void handle_fetch_result(void) {
    if (fetch_state() != FETCH_DONE) return;
    if (fetch_job_kind() == JOB_DOWNLOAD || fetch_job_kind() == JOB_UNZIP) { handle_task_result(); return; }
    if (queued) {
        /* a new page was requested while something was downloading */
        queued = false;
        if (fetch_job_kind() == JOB_IMAGE) {
            ImageResult ir;
            if (fetch_take_image(&ir)) {
                int idx = ir.tag & 0xFF;
                if (idx < page.nimages && page.images[idx].state == IMG_LOADING) page.images[idx].state = IMG_NONE;
                image_free(&ir.img);
            }
        } else {
            fetch_discard();
        }
        fetch_start(queued_url, queued_post);
        free(queued_post);
        queued_post = NULL;
        return;
    }
    if (fetch_job_kind() == JOB_IMAGE) {
        ImageResult ir;
        if (fetch_take_image(&ir)) apply_image(&ir);
        return;
    }
    if (fetch_job_kind() == JOB_CSS) {
        HttpResult r;
        int tag;
        if (!fetch_take_css(&r, &tag)) return;
        int gen = tag >> 8, idx = tag & 0xFF;
        if (gen == (page_gen & 0xFFFFFF) && has_page) {
            page_sheet_loaded(&page, idx, r.body, r.body_len);   /* takes the text */
            r.body = NULL;
        }
        http_result_free(&r);
        return;
    }
    HttpResult r;
    if (!fetch_take(&r)) return;
    if (!loading) { http_result_free(&r); return; }   /* user moved on */
    loading = false;
    if (r.download) {
        offer_download(&r);
        http_result_free(&r);
        return;
    }
    if (!r.body) {
        if (strcmp(r.error, "Stopped.") != 0) show_error(loading_url, r.error[0] ? r.error : "Unknown error.");
        restore_scroll = -1;
        return;
    }
    char ct[96];
    snprintf(ct, sizeof(ct), "%s", r.content_type);
    for (char *c = ct; *c; c++) *c = (char)tolower((unsigned char)*c);
    bool html = !ct[0] || strstr(ct, "html") || strstr(ct, "xml");
    bool plain = strstr(ct, "text/") && !html;
    if (!html && !plain) {
        char msg[200];
        char *semi = strchr(ct, ';');
        if (semi) *semi = 0;
        snprintf(msg, sizeof(msg), "This is a \"%s\" file, which this browser can't show.", ct);
        http_result_free(&r);
        show_error(r.url, msg);
        return;
    }
    diag_note(DIAG_PAGE, "%.150s", r.url);
    diag_note(DIAG_MAIN, "reading page (%u bytes)", (unsigned)r.body_len);
    free(cur_post);
    cur_post = pending_post;   /* redirects after a POST end up as GET, but re-sending is harmless here */
    pending_post = NULL;
    page_set_source(&page, r.body, r.body_len, plain, r.url);
    page.secure = r.secure;
    page.verified = r.verified;
    page.truncated = r.truncated;
    has_page = true;
    page_gen++;
    last_layout_w = 0;
    if (r.truncated) ui_toast("This page was too big and has been cut short");
    sound_play(SFX_LOADED);
}

/* ------------------------------------------------------------ keyboard */

static void address_done(bool ok, const char *text, void *user) {
    (void)user;
    if (!ok) return;
    char url[URL_MAX];
    url_from_input(text, url, sizeof(url));
    if (url[0]) navigate(url, NULL, true);
}

static void open_address_bar(void) {
    osk_open("Address or search", loading ? loading_url : (has_page ? page.url : ""), URL_MAX - 1, OSK_URL,
             address_done, NULL);
}

static void submit_form(int field, int submit_field) {
    char url[URL_MAX];
    char *post = NULL;
    if (!page_build_submit(&page, field, submit_field, url, sizeof(url), &post)) return;
    navigate(url, post, true);
    free(post);
}

static void field_done(bool ok, const char *text, void *user) {
    int packed = (int)(intptr_t)user;
    int gen = packed >> 8, f = packed & 0xFF;
    if (!ok || gen != (page_gen & 0xFFFFFF) || f >= page.nfields) return;
    snprintf(page.fields[f].value, sizeof(page.fields[f].value), "%s", text);
    /* a form with a single text box (e.g. search) submits right away */
    int texts = 0;
    for (int i = 0; i < page.nfields; i++)
        if (page.fields[i].form == page.fields[f].form &&
            (page.fields[i].type == FIELD_TEXT || page.fields[i].type == FIELD_PASSWORD)) texts++;
    if (texts == 1 && page.fields[f].form >= 0) submit_form(f, -1);
}

static void edit_field(int f) {
    Field *fd = &page.fields[f];
    char title[80];
    snprintf(title, sizeof(title), "%s", fd->type == FIELD_PASSWORD ? "Password" : (fd->name[0] ? fd->name : "Text"));
    int packed = ((page_gen & 0xFFFFFF) << 8) | (f & 0xFF);
    osk_open(title, fd->value, (int)sizeof(fd->value) - 1, fd->type == FIELD_PASSWORD ? OSK_PASSWORD : OSK_TEXT,
             field_done, (void *)(intptr_t)packed);
}

/* --------------------------------------------------------------- drawing */

static void draw_bookmarks(bool interactive) {
    const Theme *t = theme();
    float sx, sy, sw, sh;
    ui_safe_area(&sx, &sy, &sw, &sh);
    float open = ui_anim(ui_id("bm-open", 0), 1.0f, 0.25f);
    gfx_rect(0, 0, gfx_width(), 480, color_alpha(t->shadow, 0.6f * open));
    float pw = fminf(560, sw - 20), ph = 380;
    float px = sx + (sw - pw) * 0.5f, py = sy + (sh - ph) * 0.5f + (1 - open) * 60;
    ui_set_input(interactive);
    ui_panel(px, py, pw, ph, 20);
    ui_draw_icon(ICON_STAR, px + 30, py + 28, 26, t->accent);
    gfx_text(FONT_UI, 24, px + 50, py + 12, t->text, "Bookmarks", -1);
    if (ui_icon_button(ui_id("bm-close", 0), px + pw - 30, py + 28, 16, ICON_CLOSE, true)) {
        bm_open = false;
        sound_play(SFX_CLOSE);
    }
    if (ui_button(ui_id("bm-add", 0), px + 20, py + 56, pw - 40, 40, "Bookmark this page", 18, UIB_ACCENT))
        bookmark_add_current();

    const int visible = 6;
    float row_h = 38, ly = py + 108;
    if (nbookmarks == 0)
        gfx_text_centered(FONT_UI, 17, px + pw * 0.5f, ly + 60, t->text_dim, "No bookmarks yet.");
    int dir = ui_dpad_unhandled();
    float sd = input_scroll_delta();
    if ((dir == DIR_DOWN || sd > 20) && bm_scroll + visible < nbookmarks) bm_scroll++;
    if ((dir == DIR_UP || sd < -20) && bm_scroll > 0) bm_scroll--;
    if (bm_scroll > nbookmarks - visible) bm_scroll = nbookmarks > visible ? nbookmarks - visible : 0;
    for (int i = bm_scroll; i < nbookmarks && i < bm_scroll + visible; i++) {
        float y = ly + (i - bm_scroll) * (row_h + 6);
        if (ui_button(ui_id("bm-row", i), px + 20, y, pw - 90, row_h, bookmarks[i].title, 16, UIB_FLAT)) {
            bm_open = false;
            navigate(bookmarks[i].url, NULL, true);
            return;
        }
        bool confirm = (confirm_delete == i);
        if (ui_icon_button(ui_id("bm-del", i), px + pw - 45, y + row_h * 0.5f, 15, ICON_CLOSE, true)) {
            if (confirm) {
                memmove(&bookmarks[i], &bookmarks[i + 1], sizeof(Bookmark) * (size_t)(nbookmarks - i - 1));
                nbookmarks--;
                confirm_delete = -1;
                bookmarks_save();
                ui_toast("Bookmark deleted");
                return;
            }
            confirm_delete = i;
            ui_toast("Press again (or X) to delete");
        }
    }
    if (nbookmarks > visible) {
        char more[48];
        snprintf(more, sizeof(more), "%d-%d of %d", bm_scroll + 1, bm_scroll + visible, nbookmarks);
        gfx_text_centered(FONT_UI, 14, px + pw * 0.5f, py + ph - 26, t->text_dim, more);
    }
}

static void draw_clock(float right_x, float y) {
    const Theme *t = theme();
    time_t now = netio_now() + g_set.tz_minutes * 60 + (g_set.dst ? 3600 : 0);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    char buf[16];
    int h12 = tmv.tm_hour % 12;
    snprintf(buf, sizeof(buf), "%d:%02d %s", h12 ? h12 : 12, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
    float w = gfx_text_width(FONT_UI, 15, buf, -1);
    gfx_text(FONT_UI, 15, right_x - w, y, t->text, buf, -1);
}

/* Exit button: press twice (within 3 seconds) to leave the browser. */
static int exit_armed_until;

static void exit_pressed(void) {
    if (ui_frame() > exit_armed_until) {
        exit_armed_until = ui_frame() + 180;
        ui_toast(g_set.exit_action == 1 ? "Press again to restart the console" : "Press again to exit to Swiss");
        return;
    }
    exit_armed_until = 0;
    settings_save();
    cookies_save_if_needed();
    if (g_set.exit_action == 1) { system_restart(); return; }
    char err[256];
    if (!dol_check(g_set.swiss_dol, err, sizeof(err))) {
        /* not where it was set: look for a Swiss .dol and remember it */
        static char found[48][256];
        int n = storage_ok() ? dol_find("sd:/", found, 48) : 0;
        int pick = -1;
        for (int i = 0; i < n && pick < 0; i++) {
            const char *name = strrchr(found[i], '/');
            name = name ? name + 1 : found[i];
            if (!strncasecmp(name, "swiss", 5) && dol_check(found[i], err, sizeof(err))) pick = i;
        }
        if (pick < 0) {
            ui_toast("Swiss .dol not found - choose it in Settings > Data");
            sound_play(SFX_ERROR);
            return;
        }
        snprintf(g_set.swiss_dol, sizeof(g_set.swiss_dol), "%s", found[pick]);
        settings_save();
    }
    if (!dol_launch(g_set.swiss_dol, err, sizeof(err))) {
        ui_toast(err);
        sound_play(SFX_ERROR);
    }
}

void browser_init(void) {
    cookies_init();
    page_init(&page);
    bookmarks_load();
    browser_navigate(g_set.home);
}

bool browser_take_settings_request(void) {
    bool r = settings_request;
    settings_request = false;
    return r;
}

void browser_frame(bool interactive) {
    const Theme *t = theme();
    bool modal = osk_active() || bm_open || dl_state != DL_IDLE;
    handle_fetch_result();

    float sx, sy, sw, sh;
    ui_safe_area(&sx, &sy, &sw, &sh);
    ui_set_input(interactive && !modal);
    bool input_ok = interactive && !modal;

    /* ---- toolbar */
    float bar_h = 54;
    ui_panel(sx + 4, sy + 4, sw - 8, bar_h, 24);
    float by = sy + 4 + bar_h * 0.5f, r = 18;
    float bx = sx + 30;
    if (ui_icon_button(ui_id("tb", 0), bx, by, r, ICON_BACK, nback > 0)) go_back();
    if (ui_icon_button(ui_id("tb", 1), bx + 44, by, r, ICON_FORWARD, nfwd > 0)) go_forward();
    if (ui_icon_button(ui_id("tb", 2), bx + 88, by, r, loading ? ICON_STOP : ICON_RELOAD, true)) {
        if (loading) { queued = false; fetch_cancel(); }
        else if (has_page) { restore_scroll = page.scroll_target; start_load(page.url, NULL); }
    }
    if (ui_icon_button(ui_id("tb", 3), bx + 132, by, r, ICON_HOME, true)) browser_navigate(g_set.home);
    float right = sx + sw - 30;
    if (ui_icon_button(ui_id("tb", 5), right, by, r, ICON_GEAR, true)) settings_request = true;
    if (ui_icon_button(ui_id("tb", 6), right - 88, by, r, ICON_POWER, true)) exit_pressed();
    if (ui_icon_button(ui_id("tb", 4), right - 44, by, r, ICON_STAR, true)) { bm_open = true; bm_scroll = 0; confirm_delete = -1; sound_play(SFX_OPEN); }

    /* address field */
    float ax = bx + 160, aw = (right - 114) - ax, ah = 34, ay = by - ah * 0.5f;
    bool addr_hover = ui_hover(ui_id("addr", 0), ax, ay, aw, ah);
    float ahv = ui_anim(ui_id("addr", 1), addr_hover ? 1.0f : 0.0f, 0.25f);
    if (ahv > 0.01f) gfx_rrect(ax - 4, ay - 4, aw + 8, ah + 8, 21, color_alpha(t->accent, 0.45f * ahv));
    gfx_rrect(ax, ay, aw, ah, 17, t->field_bg);
    gfx_rrect_outline(ax, ay, aw, ah, 17, 1.5f, t->panel_edge);
    const char *shown = loading ? loading_url : (has_page ? page.url : "");
    float tx = ax + 14;
    if (!loading && has_page && page.secure) {
        ui_draw_icon(page.verified ? ICON_LOCK : ICON_UNLOCK, ax + 18, by, 18, page.verified ? t->good : t->bad);
        tx += 18;
    }
    gfx_set_clip(tx, ay, aw - (tx - ax) - 12, ah);
    int n = gfx_text_fit(FONT_BODY, 17, shown, -1, aw - (tx - ax) - 16);
    gfx_text(FONT_BODY, 17, tx, ay + 7, t->field_text, shown, n);
    gfx_clear_clip();
    if (addr_hover && input_ok && input_pressed(ACT_CLICK)) {
        input_consume(ACT_CLICK);
        sound_play(SFX_SELECT);
        open_address_bar();
    }

    /* ---- content area */
    float cx = sx + 8, cy = sy + bar_h + 16, cw = sw - 16, ch = sh - bar_h - 16 - 34;
    gfx_shadow(cx + 2, cy + 4, cw, ch, 14, 6, t->shadow);
    gfx_rrect(cx, cy, cw, ch, 14, has_page && page.dom ? page.bg : t->page_bg);

    float scale = g_set.text_size / 100.0f;
    static int last_layout_frame, last_load_images = -1, last_alt = -1, last_img_size = -1;
    if (last_load_images != g_set.load_images || last_alt != g_set.show_alt || last_img_size != g_set.img_size) {
        last_load_images = g_set.load_images;
        last_alt = g_set.show_alt;
        last_img_size = g_set.img_size;
        page.relayout = true;
    }
    if (page.styled_author != (g_set.page_styles != 0)) page.restyle = true;
    bool image_relayout = page.relayout && ui_frame() - last_layout_frame >= 90;
    if (has_page && (last_layout_w != cw || last_layout_scale != scale || image_relayout || page.restyle)) {
        last_layout_frame = ui_frame();
        diag_note(DIAG_MAIN, "laying out page (%d pictures ready)", page.nimages);
        page_layout(&page, cw, scale);
        last_layout_w = cw;
        last_layout_scale = scale;
        if (restore_scroll >= 0) { page.scroll = page.scroll_target = restore_scroll; restore_scroll = -1; }
    }

    int hover_link = -1, hover_field = -1;
    const char *home_hover = NULL;
    bool on_home = has_page && !strcmp(page.url, "about:home");
    if (on_home) {
        /* the start page draws itself (home.c) */
        const char *go = NULL;
        HomeAction ha = home_draw(cx, cy, cw, ch, input_ok, &go, &home_hover);
        if (ha != HOME_NONE) sound_play(ha == HOME_BOOKMARKS ? SFX_OPEN : SFX_SELECT);
        switch (ha) {
        case HOME_OPEN_URL: navigate(go, NULL, true); break;
        case HOME_SEARCH: open_address_bar(); break;
        case HOME_BOOKMARKS: bm_open = true; bm_scroll = 0; confirm_delete = -1; break;
        case HOME_HELP: navigate("about:help", NULL, true); break;
        case HOME_SETTINGS: settings_request = true; break;
        default: break;
        }
    } else if (has_page) {
        /* scrolling */
        if (input_ok) {
            page.scroll_target += input_scroll_delta();
            int d = ui_dpad_unhandled();
            if (d == DIR_DOWN) page.scroll_target += 120;
            if (d == DIR_UP) page.scroll_target -= 120;
        }
        float max_scroll = page.content_h - ch;
        if (max_scroll < 0) max_scroll = 0;
        if (page.scroll_target > max_scroll) page.scroll_target = max_scroll;
        if (page.scroll_target < 0) page.scroll_target = 0;
        page.scroll += (page.scroll_target - page.scroll) * (g_set.animations ? 0.3f : 1.0f);
        schedule_images(page.scroll, ch, fminf(cw - 32, 640));

        /* register visible links/fields as targets and find the hovered one */
        float px = input_cursor_x(), py = input_cursor_y();
        for (int i = 0; i < page.nboxes; i++) {
            Box *b = &page.boxes[i];
            if (b->link < 0 && b->field < 0) continue;
            float yy = cy + b->y - page.scroll;
            if (yy + b->h < cy || yy > cy + ch) continue;
            float y0 = fmaxf(yy, cy), y1 = fminf(yy + b->h, cy + ch);
            if (ui_hover(ui_id("pg", i), cx + b->x, y0, b->w, y1 - y0)) {
                hover_link = b->link;
                hover_field = b->field;
            }
            (void)px; (void)py;
        }
        diag_note(DIAG_MAIN, "drawing page at scroll %.0f of %.0f (%d boxes)", page.scroll, page.content_h, page.nboxes);
        diag_note(DIAG_DRAW, "page at scroll %.0f of %.0f, %d boxes, %d pictures", page.scroll, page.content_h, page.nboxes, page.nimages);
        page_draw(&page, cx, cy, cw, ch, hover_link, hover_field);
        diag_note(DIAG_MAIN, "drawing toolbar and status bar");

        /* scroll bar */
        if (page.content_h > ch) {
            float bh = fmaxf(30, ch * ch / page.content_h);
            float byy = cy + (ch - bh) * (page.scroll / (page.content_h - ch));
            gfx_rrect(cx + cw - 9, cy + 4, 5, ch - 8, 2.5f, color_alpha(t->page_rule, 0.6f));
            gfx_rrect(cx + cw - 9, byy + 4, 5, bh - 8, 2.5f, t->accent);
        }

        if (input_ok && input_pressed(ACT_CLICK) && (hover_link >= 0 || hover_field >= 0)) {
            input_consume(ACT_CLICK);
            input_rumble(3);
            sound_play(SFX_SELECT);
            if (hover_field >= 0) {
                int ft = page.fields[hover_field].type;
                if (ft == FIELD_SUBMIT) submit_form(hover_field, hover_field);
                else if (ft == FIELD_CHECK || ft == FIELD_RADIO) page_toggle_field(&page, hover_field);
                else if (ft == FIELD_SELECT) ui_toast("Changing drop-down choices isn't supported yet");
                else edit_field(hover_field);
            } else if (page_link_is_download(&page, hover_link) || input_held(ACT_SLOW)) {
                save_link(page_link_url(&page, hover_link));
            } else {
                navigate(page_link_url(&page, hover_link), NULL, true);
            }
        }
    }
    if (loading) {
        if (!has_page) ui_spinner(cx + cw * 0.5f, cy + ch * 0.5f, 60);
        float k = fmodf(ui_time() * 0.8f, 1.0f);
        gfx_rect(cx + 10, cy + 2, cw - 20, 3, color_alpha(t->page_rule, 0.7f));
        gfx_rect(cx + 10 + (cw - 20) * fmaxf(0, k - 0.25f), cy + 2, (cw - 20) * fminf(0.25f, k), 3, t->accent);
    }

    /* ---- status bar */
    float sbx = sx + 14, sby = sy + sh - 28;
    char status[300];
    if (loading) {
        int kb = fetch_progress() / 1024;
        if (kb > 0) snprintf(status, sizeof(status), "%s %d KB", fetch_stage(), kb);
        else snprintf(status, sizeof(status), "%s", fetch_stage());
        ui_spinner(sbx + 8, sby + 10, 14);
        sbx += 24;
    } else if (hover_link >= 0) {
        snprintf(status, sizeof(status), "%s", page_link_url(&page, hover_link));
    } else if (home_hover) {
        snprintf(status, sizeof(status), "%s", home_hover);
    } else if (has_page && sheets_pending() && netio_state() == NET_UP) {
        snprintf(status, sizeof(status), "Loading page styles...");
        ui_spinner(sbx + 8, sby + 10, 14);
        sbx += 24;
    } else if (has_page && g_set.load_images && images_pending() > 0 && netio_state() == NET_UP) {
        snprintf(status, sizeof(status), "Loading pictures (%d left)", images_pending());
        ui_spinner(sbx + 8, sby + 10, 14);
        sbx += 24;
    } else if (netio_state() != NET_UP) {
        snprintf(status, sizeof(status), "%s", fetch_stage()[0] ? fetch_stage() : netio_status());
    } else {
        snprintf(status, sizeof(status), "%s", has_page && page.title[0] ? page.title : "Ready");
    }
    float status_w = sw - 150 - (sbx - sx);
    int sn = gfx_text_fit(FONT_UI, 15, status, -1, status_w);
    gfx_text(FONT_UI, 15, sbx, sby, t->text, status, sn);
    Color dot = netio_state() == NET_UP ? t->good : (netio_state() == NET_FAILED ? t->bad : t->accent);
    gfx_circle(sx + sw - 110, sby + 10, 5, dot);
    draw_clock(sx + sw - 14, sby);

    /* ---- shortcuts */
    if (input_ok) {
        if (input_pressed(ACT_BACK)) { input_consume(ACT_BACK); go_back(); }
        if (input_pressed(ACT_ADDRESS)) { input_consume(ACT_ADDRESS); open_address_bar(); }
        if (input_pressed(ACT_BOOKMARKS)) {
            input_consume(ACT_BOOKMARKS);
            bm_open = true; bm_scroll = 0; confirm_delete = -1;
            sound_play(SFX_OPEN);
        }
        if (input_pressed(ACT_MENU)) { input_consume(ACT_MENU); settings_request = true; }
    }

    /* ---- modal layers */
    if (dl_state != DL_IDLE) draw_download(interactive && !osk_active());
    if (bm_open) {
        bool bm_input = interactive && !osk_active();
        draw_bookmarks(bm_input);
        if (bm_input && (input_pressed(ACT_BACK) || input_pressed(ACT_BOOKMARKS))) {
            input_consume(ACT_BACK);
            input_consume(ACT_BOOKMARKS);
            bm_open = false;
            sound_play(SFX_CLOSE);
        }
        if (bm_input && (input_raw_pressed() & PAD_BUTTON_X) && confirm_delete >= 0) {
            int i = confirm_delete;
            memmove(&bookmarks[i], &bookmarks[i + 1], sizeof(Bookmark) * (size_t)(nbookmarks - i - 1));
            nbookmarks--;
            confirm_delete = -1;
            bookmarks_save();
            ui_toast("Bookmark deleted");
        }
    }
}
