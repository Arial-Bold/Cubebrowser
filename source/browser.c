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
#include "sound.h"
#include "url.h"
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
#define BOOKMARK_FILE STORAGE_DIR "/bookmarks.txt"

typedef struct { char url[URL_MAX]; float scroll; } HistEntry;
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

static const char home_html[] =
    "<title>Start</title>"
    "<h1>Welcome to CubeBrowser</h1>"
    "<p>Press <b>X</b> to type an address or search. Point with the Control Stick and press <b>A</b> "
    "to follow links. The <b>D-Pad</b> jumps between links, and the <b>C-Stick</b> scrolls.</p>"
    "<h3>Light sites that work well</h3><ul>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a> - search</li>"
    "<li><a href=\"https://en.m.wikipedia.org/wiki/GameCube\">Wikipedia (mobile)</a> - encyclopedia</li>"
    "<li><a href=\"https://text.npr.org/\">NPR Text</a> - news</li>"
    "<li><a href=\"https://lite.cnn.com/\">CNN Lite</a> - news</li>"
    "<li><a href=\"http://68k.news/\">68k.news</a> - headlines made for vintage computers</li>"
    "<li><a href=\"http://frogfind.com/\">FrogFind</a> - search that simplifies pages</li>"
    "<li><a href=\"https://wiby.me/\">Wiby</a> - a search engine for classic web pages</li>"
    "</ul><p><a href=\"about:help\">Controls and tips</a></p>";

static const char help_html[] =
    "<title>Controls</title><h1>Controls</h1><ul>"
    "<li><b>Control Stick</b> - move the pointer (hold Z for fine control)</li>"
    "<li><b>A</b> - click</li><li><b>B</b> - back</li>"
    "<li><b>X</b> - type an address or search</li><li><b>Y</b> - bookmarks</li>"
    "<li><b>START</b> - settings</li><li><b>C-Stick</b> - scroll</li>"
    "<li><b>L / R</b> - page up / page down</li>"
    "<li><b>D-Pad</b> - jump between links and buttons</li></ul>"
    "<p>All buttons except the D-Pad can be changed in Settings &gt; Controller.</p>"
    "<h3>On-screen keyboard</h3><ul><li><b>B</b> delete, <b>Y</b> space, <b>X</b> shift</li>"
    "<li><b>L / R</b> move the text cursor, <b>START</b> accept</li></ul>"
    "<h3>What works</h3><p>Text, links, headings, lists and simple forms such as search boxes. "
    "Pictures, styles and JavaScript are not supported, so lightweight and text-only sites work best.</p>"
    "<p><a href=\"about:home\">Back to the start page</a></p>";

/* ------------------------------------------------------------ bookmarks */

static void bookmarks_load(void) {
    nbookmarks = 0;
    size_t len;
    char *data = storage_read(BOOKMARK_FILE, &len);
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
    if (!storage_write_atomic(BOOKMARK_FILE, STORAGE_DIR "/bookmarks.tmp", buf, n))
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

static void push_history(void) {
    if (!has_page) return;
    if (nback == HIST_MAX) { memmove(back_stack, back_stack + 1, sizeof(HistEntry) * (HIST_MAX - 1)); nback--; }
    snprintf(back_stack[nback].url, URL_MAX, "%s", page.url);
    back_stack[nback].scroll = page.scroll_target;
    nback++;
}

static void start_load(const char *url, const char *post) {
    if (!strncasecmp(url, "about:", 6)) {
        if (loading) { queued = false; fetch_cancel(); loading = false; }
        if (!strcasecmp(url, "about:home") || !strcasecmp(url, "about:blank") || !strcasecmp(url, "about:"))
            show_html(home_html, "about:home", false);
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
        nfwd = 0;
    }
    start_load(url, post);
}

void browser_navigate(const char *url) { navigate(url, NULL, true); }

static void go_back(void) {
    if (nback == 0) { sound_play(SFX_ERROR); return; }
    if (has_page && nfwd < HIST_MAX) {
        snprintf(fwd_stack[nfwd].url, URL_MAX, "%s", page.url);
        fwd_stack[nfwd++].scroll = page.scroll_target;
    }
    HistEntry e = back_stack[--nback];
    restore_scroll = e.scroll;
    sound_play(SFX_BACK);
    start_load(e.url, NULL);
}

static void go_forward(void) {
    if (nfwd == 0) return;
    push_history();
    HistEntry e = fwd_stack[--nfwd];
    restore_scroll = e.scroll;
    start_load(e.url, NULL);
}

void browser_clear_history(void) { nback = nfwd = 0; }

static void handle_fetch_result(void) {
    HttpResult r;
    if (!fetch_take(&r)) return;
    if (queued) {
        queued = false;
        http_result_free(&r);
        fetch_start(queued_url, queued_post);
        free(queued_post);
        queued_post = NULL;
        return;
    }
    if (!loading) { http_result_free(&r); return; }   /* user moved on */
    loading = false;
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

void browser_init(void) {
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
    bool modal = osk_active() || bm_open;
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
    if (ui_icon_button(ui_id("tb", 4), right - 44, by, r, ICON_STAR, true)) { bm_open = true; bm_scroll = 0; confirm_delete = -1; sound_play(SFX_OPEN); }

    /* address field */
    float ax = bx + 160, aw = (right - 70) - ax, ah = 34, ay = by - ah * 0.5f;
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
    gfx_rrect(cx, cy, cw, ch, 14, t->page_bg);

    float scale = g_set.text_size / 100.0f;
    if (has_page && (last_layout_w != cw || last_layout_scale != scale)) {
        page_layout(&page, cw, scale);
        last_layout_w = cw;
        last_layout_scale = scale;
        if (restore_scroll >= 0) { page.scroll = page.scroll_target = restore_scroll; restore_scroll = -1; }
    }

    int hover_link = -1, hover_field = -1;
    if (has_page) {
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
        page_draw(&page, cx, cy, cw, ch, hover_link, hover_field);

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
                else edit_field(hover_field);
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
