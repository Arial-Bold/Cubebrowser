/* player.c - see player.h. Three tabs (Playing, Songs, Sounds), plus the
 * screens shown while linking up and when the cable is unplugged. */
#include "player.h"
#include "gfx.h"
#include "gbaproto.h"
#include <string.h>

#define MAX_SONGS 400
#define NAME_MAX 40

/* colours (from the browser's purple theme) */
#define BG0   RGB8(28, 18, 74)
#define BG1   RGB8(70, 48, 150)
#define BAR   RGB8(60, 44, 138)
#define BARL  RGB8(120, 104, 220)
#define EDGE  RGB8(220, 212, 255)
#define ACC   RGB8(255, 214, 92)
#define TXT   RGB8(255, 255, 255)
#define DIM   RGB8(214, 206, 250)
#define DIM2  RGB8(170, 160, 230)
#define ONACC RGB8(60, 36, 120)
#define FIELD RGB8(246, 244, 255)
#define FTXT  RGB8(40, 30, 90)
#define FDIM  RGB8(110, 104, 140)
#define ROW   RGB8(110, 94, 205)
#define ROWL  RGB8(150, 136, 235)
#define BTN   RGB8(120, 104, 220)
#define TRACK RGB8(225, 220, 245)
#define VOLOFF RGB8(110, 94, 200)
#define HINT  RGB8(22, 14, 58)
#define GOOD  RGB8(90, 210, 120)
#define BAD   RGB8(255, 130, 130)
#define DECO  RGB8(72, 54, 155)

/* what the GameCube told us */
static GbpState st;
static bool have_state;
static char title[GBP_TEXT_MAX + 1], instr[GBP_TEXT_MAX + 1], msg[GBP_TEXT_MAX + 1];
static EWRAM_BSS char songs[MAX_SONGS][NAME_MAX + 1];
static EWRAM_BSS bool song_known[MAX_SONGS];
static char sf_name[GBP_SF_MAX][NAME_MAX + 1];
static uint16_t sf_size[GBP_SF_MAX];
static bool sf_known[GBP_SF_MAX];

/* our own */
static int tab, sel = 4, song_cur, song_top, sf_cur_row, sf_top;
static int link_state = LINK_WAIT;
static bool dirty = true, screen_off;
static unsigned frames, last_song_frame;

void player_init(void) {
    memset(&st, 0, sizeof(st));
    st.cur = 0xFFFF;
    have_state = false;
    memset(song_known, 0, sizeof(song_known));   /* (this memory isn't cleared at start) */
    memset(sf_known, 0, sizeof(sf_known));
    dirty = true;
}

static void copy_text(char *dst, const uint8_t *d, int len) {
    if (len > GBP_TEXT_MAX) len = GBP_TEXT_MAX;
    memcpy(dst, d, (size_t)len);
    dst[len] = 0;
}

void player_message(int type, const uint8_t *d, int len) {
    switch (type) {
    case GBP_HELLO:
        break;
    case GBP_STATE:
        if (len >= GBP_STATE_BYTES) {
            GbpState n;
            gbp_state_unpack(&n, d);
            if (n.count != st.count)
                for (int i = 0; i < MAX_SONGS; i++) song_known[i] = false;
            if (n.sf_count != st.sf_count)
                for (int i = 0; i < GBP_SF_MAX; i++) sf_known[i] = false;
            st = n;
            if (!have_state) { song_cur = st.cur < st.count ? st.cur : 0; sf_cur_row = st.sf_cur; }
            have_state = true;
        }
        break;
    case GBP_TITLE: copy_text(title, d, len); break;
    case GBP_INSTR: copy_text(instr, d, len); break;
    case GBP_MSG: copy_text(msg, d, len); break;
    case GBP_SONG:
        if (len >= 2) {
            int i = (int)gbp_get16(d);
            if (i < MAX_SONGS) {
                int n = len - 2 > NAME_MAX ? NAME_MAX : len - 2;
                memcpy(songs[i], d + 2, (size_t)n);
                songs[i][n] = 0;
                song_known[i] = true;
                last_song_frame = frames;
            }
        }
        break;
    case GBP_SF:
        if (len >= 3 && d[0] < GBP_SF_MAX) {
            int i = d[0];
            sf_size[i] = (uint16_t)gbp_get16(d + 1);
            int n = len - 3 > NAME_MAX ? NAME_MAX : len - 3;
            memcpy(sf_name[i], d + 3, (size_t)n);
            sf_name[i][n] = 0;
            sf_known[i] = true;
        }
        break;
    default:
        return;
    }
    dirty = true;
}

void player_link(int s) {
    if (s != link_state) { link_state = s; dirty = true; }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void enter_tab(int t) {
    tab = t;
    if (tab == 1 && st.cur < st.count) song_cur = st.cur;
    if (tab == 2 && st.sf_cur < st.sf_count) sf_cur_row = st.sf_cur;
    dirty = true;
}

void player_input(unsigned pressed, unsigned repeat) {
    if (screen_off) {
        if (pressed) { screen_off = false; player_screen_power(true); dirty = true; }
        return;
    }
    if (pressed & K_SELECT) { screen_off = true; player_screen_power(false); return; }
    if (link_state != LINK_OK || !have_state) return;
    unsigned dir = pressed | repeat;
    if (pressed & K_START) player_send(GBC_TOGGLE, 0);
    if (pressed & K_L) enter_tab((tab + 2) % 3);
    if (pressed & K_R) enter_tab((tab + 1) % 3);
    if (pressed & (K_L | K_R)) return;

    if (tab == 0) {
        if (dir & K_LEFT) sel = clampi(sel - 1, 0, 5);
        if (dir & K_RIGHT) sel = clampi(sel + 1, 0, 5);
        if (dir & (K_UP | K_DOWN)) {
            int v = clampi(st.volume + ((dir & K_UP) ? 1 : -1), 0, 10);
            if (v != st.volume) { st.volume = (uint8_t)v; player_send(GBC_VOLUME, v); }
        }
        if (pressed & K_A) {
            switch (sel) {
            case 0: st.flags ^= GBF_SHUFFLE; player_send(GBC_SHUFFLE, (st.flags & GBF_SHUFFLE) != 0); break;
            case 1: st.flags ^= GBF_REPEAT; player_send(GBC_REPEAT, (st.flags & GBF_REPEAT) != 0); break;
            case 2: player_send(GBC_STOP, 0); break;
            case 3: player_send(GBC_PREV, 0); break;
            case 4: player_send(GBC_TOGGLE, 0); break;
            case 5: player_send(GBC_NEXT, 0); break;
            }
        }
        if (pressed & K_B) player_send(GBC_STOP, 0);
    } else if (tab == 1) {
        int n = st.count;
        if (n > 0) {
            if (dir & K_UP) song_cur--;
            if (dir & K_DOWN) song_cur++;
            if (dir & K_LEFT) song_cur -= 7;
            if (dir & K_RIGHT) song_cur += 7;
            song_cur = clampi(song_cur, 0, n - 1);
            if (pressed & K_A) player_send(GBC_PLAY, song_cur);
        }
        if (pressed & K_B) enter_tab(0);
    } else {
        int n = st.sf_count;
        if (n > 0) {
            if (dir & K_UP) sf_cur_row--;
            if (dir & K_DOWN) sf_cur_row++;
            sf_cur_row = clampi(sf_cur_row, 0, n - 1);
            if ((pressed & K_A) && sf_cur_row != st.sf_cur) player_send(GBC_INSTR, sf_cur_row);
        }
        if (pressed & K_B) enter_tab(0);
    }
    if (pressed | repeat) dirty = true;
}

void player_frame(void) {
    frames++;
    bool animated = link_state != LINK_OK || !have_state || st.state == GBS_LOADING;
    if (animated && frames % 6 == 0) dirty = true;
    /* the time moves on by itself between updates from the GameCube */
}

bool player_needs_draw(void) { return dirty && !screen_off; }

int player_missing_song(void) {
    if (!have_state || frames - last_song_frame < 60) return -1;   /* (they may still be on the way) */
    for (int i = 0; i < st.count && i < MAX_SONGS; i++) if (!song_known[i]) return i;
    return -1;
}

/* ------------------------------------------------------------- drawing */

static void background(bool deco) {
    gfx_vgrad(0, SCR_H, BG0, BG1);
    if (deco) {
        static const int sq[3][3] = { { 200, 120, 30 }, { 30, 70, 22 }, { 170, 40, 16 } };
        for (int i = 0; i < 3; i++) {
            int cx = sq[i][0], cy = sq[i][1], s = sq[i][2];
            gfx_fill(cx - s, cy - s, 2 * s + 1, 1, DECO);
            gfx_fill(cx - s, cy + s, 2 * s + 1, 1, DECO);
            gfx_fill(cx - s, cy - s, 1, 2 * s + 1, DECO);
            gfx_fill(cx + s, cy - s, 1, 2 * s + 1, DECO);
        }
    }
}

static void tabs(void) {
    gfx_rrect_line(3, 2, 234, 18, 8, BARL, BAR);
    static const char *const names[3] = { "Playing", "Songs", "Sounds" };
    /* the tabs sit between the L and R marks, so the highlight never covers them */
    for (int i = 0; i < 3; i++) {
        int x0 = 18 + i * 69;
        if (i == tab) gfx_rrect(x0, 4, 67, 14, 6, ACC);
        gfx_text_c(&font9b, x0 + 33, 5, i == tab ? ONACC : DIM, names[i]);
    }
    gfx_text(&font8b, 8, 5, ROWL, "L", 0);
    gfx_text(&font8b, 226, 5, ROWL, "R", 0);
}

static void hints(const char *t) {
    gfx_fill(0, SCR_H - 13, SCR_W, 13, HINT);
    gfx_text_c(&font8b, SCR_W / 2, SCR_H - 12, DIM, t);
}

/* tiny text helpers (no printf, to keep the program small to send) */
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *put_u(char *p, unsigned v) {
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}
static void fmt_time(char *o, unsigned s) {
    o = put_u(o, s / 60);
    *o++ = ':';
    *o++ = (char)('0' + (s % 60) / 10);
    *o++ = (char)('0' + s % 10);
    *o = 0;
}

static const char *song_name(int i) {
    if (i < 0 || i >= st.count || i >= MAX_SONGS) return "";
    return song_known[i] ? songs[i] : "...";
}

static void button(int cx, int cy, bool big, bool picked, const Mask *icon, Col icol) {
    if (big) {
        if (picked) gfx_mask(cx - 15, cy - 15, &m_circle15, ACC);
        gfx_mask(cx - 13, cy - 13, &m_circle13, EDGE);
        gfx_mask(cx - 12, cy - 12, &m_circle12, BTN);
    } else {
        if (picked) gfx_mask(cx - 12, cy - 12, &m_circle12, ACC);
        gfx_mask(cx - 10, cy - 10, &m_circle10, EDGE);
        gfx_mask(cx - 9, cy - 9, &m_circle9, BTN);
    }
    gfx_mask(cx - 6, cy - 6, icon, icol);
}

static void draw_playing(void) {
    tabs();
    gfx_rrect_line(6, 23, 228, 48, 6, EDGE, FIELD);
    const char *what = st.state == GBS_PLAYING ? "Now playing" : st.state == GBS_PAUSED ? "Paused"
                     : st.state == GBS_LOADING ? "Loading..." : "Stopped";
    gfx_text(&font8b, 12, 25, FDIM, what, 0);
    const char *t = title[0] ? title : "Choose a song";
    gfx_text(&font14b, 12, 33, FTXT, t, 214);
    char line[64], a[12], b[12];
    put_s(put_s(line, "Sounds: "), instr);
    gfx_text(&font8, 12, 51, FDIM, line, 150);
    fmt_time(a, st.pos);
    fmt_time(b, st.len);
    put_s(put_s(put_s(line, a), " / "), b);
    gfx_text(&font8b, 228 - gfx_text_w(&font8b, line), 51, FTXT, line, 0);
    gfx_fill(12, 62, 216, 4, TRACK);
    if (st.len) gfx_fill(12, 62, clampi(216 * st.pos / st.len, 0, 216), 4, ACC);

    static const int xs[6] = { 22, 50, 78, 112, 148, 184 };
    const Mask *icons[6] = { &m_shuffle, &m_repeat, &m_stop, &m_prev,
                             st.state == GBS_PLAYING ? &m_pause : &m_play, &m_next };
    for (int i = 0; i < 6; i++) {
        Col ic = TXT;
        if (i == 0 && (st.flags & GBF_SHUFFLE)) ic = ACC;
        if (i == 1 && (st.flags & GBF_REPEAT)) ic = ACC;
        button(xs[i], 88, i == 4, i == sel, icons[i], ic);
    }
    gfx_text(&font8b, 204, 77, DIM, "VOL", 0);
    for (int i = 0; i < 10; i++) {
        int h = 3 + i, x = 202 + i * 3;
        gfx_fill(x, 104 - h, 2, h + 1, i < st.volume ? ACC : VOLOFF);
    }
    gfx_mask(8, 119, &m_dot5, GOOD);
    gfx_text(&font8, 17, 116, DIM, "Linked to CubeBrowser", 0);
    if (st.count) {
        if (st.cur < st.count) put_u(put_s(put_u(put_s(line, "Song "), st.cur + 1u), " of "), st.count);
        else put_s(put_u(line, st.count), " songs");
        gfx_text(&font8, 232 - gfx_text_w(&font8, line), 116, DIM, line, 0);
    }
    if (msg[0]) gfx_text(&font8, 8, 130, BAD, msg, 224);
    else if (st.flags & GBF_SHUFFLE) gfx_text(&font8, 8, 130, DIM2, "Shuffle is on", 0);
    else if (st.cur < st.count && st.count > 1) {
        int n = st.cur + 1 < st.count ? st.cur + 1 : ((st.flags & GBF_REPEAT) ? 0 : -1);
        if (n >= 0) {
            put_s(put_s(line, "Next: "), song_name(n));
            gfx_text(&font8, 8, 130, DIM2, line, 224);
        }
    }
    hints("\x7f\x80 pick   A press   \x81\x82 volume   START play");
}

static void draw_songs(void) {
    tabs();
    if (!st.count) {
        gfx_text_c(&font9b, 120, 62, TXT, "No songs yet");
        gfx_text_c(&font8, 120, 80, DIM, "Put .mid files in the midi folder");
        gfx_text_c(&font8, 120, 92, DIM, "on your SD card.");
        hints("L/R tabs");
        return;
    }
    const int vis = 7;
    if (song_cur < song_top) song_top = song_cur;
    if (song_cur >= song_top + vis) song_top = song_cur - vis + 1;
    song_top = clampi(song_top, 0, st.count > vis ? st.count - vis : 0);
    for (int r = 0; r < vis && song_top + r < st.count; r++) {
        int i = song_top + r, y = 24 + r * 17;
        bool s = i == song_cur;
        if (s) gfx_rrect(5, y - 1, 222, 16, 5, ACC);
        else gfx_rrect_line(6, y, 220, 14, 5, ROWL, ROW);
        Col c = s ? ONACC : TXT;
        if (i == st.cur) gfx_mask(10, y + 1, &m_note, s ? ONACC : ACC);
        gfx_text(&font9b, 25, y + 2, c, song_name(i), 196);
    }
    if (st.count > vis) {
        gfx_rrect(231, 24, 5, 117, 3, BAR);
        int th = 117 * vis / st.count;
        if (th < 10) th = 10;
        int ty = 24 + (117 - th) * song_top / (st.count - vis);
        gfx_rrect(231, ty, 5, th, 3, DIM);
    }
    hints("\x81\x82 move   \x7f\x80 page   A play   B back");
}

static void draw_sounds(void) {
    tabs();
    gfx_text(&font8, 8, 22, DIM, "Choose the instruments for your music", 0);
    const int vis = 4;
    int n = st.sf_count;
    if (sf_cur_row < sf_top) sf_top = sf_cur_row;
    if (sf_cur_row >= sf_top + vis) sf_top = sf_cur_row - vis + 1;
    sf_top = clampi(sf_top, 0, n > vis ? n - vis : 0);
    for (int r = 0; r < vis && sf_top + r < n; r++) {
        int i = sf_top + r, y = 35 + r * 27;
        bool s = i == sf_cur_row;
        if (s) gfx_rrect(5, y - 1, 230, 24, 6, ACC);
        else gfx_rrect_line(6, y, 228, 22, 6, ROWL, ROW);
        Col c = s ? ONACC : TXT, cd = s ? ONACC : DIM;
        gfx_mask(11, y + 6, &m_ring9, c);
        if (i == st.sf_cur) gfx_mask(13, y + 8, &m_dot5r, c);
        gfx_text(&font9b, 25, y + 1, c, sf_known[i] ? sf_name[i] : "...", 200);
        char sub[48];
        if (i == 0) put_s(sub, "the first SoundFont in the midi folder");
        else if (i == 1) put_s(sub, "small, always there");
        else if (sf_size[i] >= 10) put_s(put_u(sub, (sf_size[i] + 5) / 10u), " MB");
        else put_s(put_u(sub, sf_size[i] * 100u), " KB");
        gfx_text(&font8, 25, y + 11, cd, sub, 200);
    }
    hints("\x81\x82 move   A choose   B back");
}

static void draw_loading(void) {
    gfx_rrect_line(30, 60, 180, 46, 8, EDGE, BAR);
    gfx_text_c(&font9b, 120, 68, TXT, "Loading instruments...");
    gfx_rrect(46, 87, 148, 6, 3, ROW);
    int span = 148 - 40, p = (int)(frames % 120);
    int x = p < 60 ? p * span / 60 : (120 - p) * span / 60;
    gfx_rrect(46 + x, 87, 40, 6, 3, ACC);
}

static void draw_connecting(void) {
    background(true);
    gfx_mask(98, 26, &m_cube_top, RGB8(190, 176, 255));
    gfx_mask(98, 26, &m_cube_left, BTN);
    gfx_mask(98, 26, &m_cube_right, RGB8(90, 74, 190));
    gfx_text_c(&font14b, 120, 80, ACC, "CubeBrowser");
    gfx_text_c(&font9b, 120, 99, TXT, "Music Player");
    gfx_text_c(&font8, 120, 120, DIM, "Talking to the GameCube...");
    for (int i = 0; i < 3; i++)
        gfx_mask(110 + i * 8, 137, &m_dot5, (int)(frames / 15 % 3) == i ? ACC : ROW);
}

static void draw_unplugged(void) {
    background(true);
    gfx_rrect_line(20, 30, 200, 98, 10, EDGE, BAR);
    gfx_text_c(&font12b, 120, 42, ACC, "Link cable unplugged");
    gfx_text_c(&font8, 120, 66, TXT, "Plug it back into port 2 and the");
    gfx_text_c(&font8, 120, 79, TXT, "player picks up where it left off.");
    gfx_text_c(&font8, 120, 95, DIM, "The music keeps playing on the TV.");
}

void player_draw(void) {
    dirty = false;
    if (link_state == LINK_LOST) { draw_unplugged(); return; }
    if (link_state != LINK_OK || !have_state) { draw_connecting(); return; }
    background(false);
    if (tab == 0) draw_playing();
    else if (tab == 1) draw_songs();
    else draw_sounds();
    if (st.state == GBS_LOADING) draw_loading();
}
