/* gbalink.c - see gbalink.h.
 *
 * A thread looks after the cable: every half second it checks what's on
 * port 2. A GBA waiting for a program (switched on with no game) is sent the
 * player (gba/, built into the browser as gbaplayer_rom); a GBA already
 * running the player (plugged back in) is simply picked up again. While
 * linked, the thread swaps one 32-bit word each way at a time (gbaproto.h):
 * every millisecond while there's something to send, otherwise about 60
 * times a second, which costs next to nothing.
 *
 * The main thread (gbalink_frame) decides what to send - the player's state,
 * titles, the song list, the instrument choices - and carries out the
 * buttons pressed on the GBA, so the music player is only ever touched from
 * one thread. */
#include "gbalink.h"
#include "gbaproto.h"
#include "midi.h"
#include "settings.h"
#include "storage.h"
#include "thr.h"
#include "ui.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* the player program (made by the build from gba/; may be missing in test builds) */
extern const unsigned char gbaplayer_rom[] __attribute__((weak));
extern const unsigned int gbaplayer_rom_size __attribute__((weak));

/* ============================================================ the cable */

#ifdef GEKKO
#include <gccore.h>
#include <ogc/si.h>
#include <ogc/semaphore.h>

#define PORT 1                 /* controller port 2 */
static sem_t si_sem;
static volatile u32 si_ret;
static u8 si_out[8] __attribute__((aligned(32)));
static u8 si_in[8] __attribute__((aligned(32)));

static void si_done(s32 chan, u32 ret) { (void)chan; si_ret = ret; LWP_SemPost(si_sem); }

/* One JOY BUS command; waits (sleeping) for the answer. */
static bool xfer(u32 out_len, u32 in_len) {
    int tries = 0;
    while (!SI_Transfer(PORT, si_out, out_len, si_in, in_len, si_done, 50)) {   /* channel busy (controller polling) */
        if (++tries > 20) return false;
        thr_sleep_ms(1);
    }
    struct timespec t = { 0, 30 * 1000 * 1000 };
    if (LWP_SemTimedWait(si_sem, &t) != 0) return false;
    return (si_ret & 0x0F) == 0;   /* no under/overrun, collision or missing answer */
}

static bool hw_is_gba(void) {
    u32 t = SI_GetType(PORT);
    if (t & SI_ERROR_BUSY) { thr_sleep_ms(20); t = SI_GetType(PORT); }
    return !(t & (SI_ERROR_BUSY | SI_ERROR_NO_RESPONSE)) && SI_DecodeType(t) == SI_GBA;
}
static bool hw_status(u8 *st) { si_out[0] = 0x00; if (!xfer(1, 3)) return false; *st = si_in[2]; return true; }
static bool hw_reset(u8 *st) { si_out[0] = 0xFF; if (!xfer(1, 3)) return false; *st = si_in[2]; return true; }
static bool hw_read(uint32_t *w) {
    si_out[0] = 0x14;
    if (!xfer(1, 5)) return false;
    *w = si_in[0] | ((u32)si_in[1] << 8) | ((u32)si_in[2] << 16) | ((u32)si_in[3] << 24);
    return true;
}
static bool hw_write(uint32_t w) {
    si_out[0] = 0x15;
    si_out[1] = (u8)w; si_out[2] = (u8)(w >> 8); si_out[3] = (u8)(w >> 16); si_out[4] = (u8)(w >> 24);
    return xfer(5, 1);
}

/* The GBA's start-up program accepts a program over the cable when it's
 * switched on with no game inserted; the exchange below (keys, encryption
 * and checksum) is the one it expects. */
static uint32_t boot_crc(uint32_t crc, uint32_t val) {
    for (int i = 0; i < 32; i++) {
        if ((crc ^ val) & 1) crc = (crc >> 1) ^ 0xA1C1;
        else crc >>= 1;
        val >>= 1;
    }
    return crc;
}

static uint32_t boot_key(uint32_t size) {
    uint32_t ret = 0;
    size = (size - 0x200) >> 3;
    int res1 = (int)((size & 0x3F80) << 1);
    res1 |= (int)((size & 0x4000) << 2);
    res1 |= (int)(size & 0x7F);
    res1 |= 0x380000;
    int res2 = res1;
    res1 = res2 >> 0x10;
    int res3 = res2 >> 8;
    res3 += res1;
    res3 += res2;
    res3 <<= 24;
    res3 |= res2;
    res3 |= (int)0x80808080;
    if ((res3 & 0x200) == 0) {
        ret |= (((res3) & 0xFF) ^ 0x4B) << 24;
        ret |= (((res3 >> 8) & 0xFF) ^ 0x61) << 16;
        ret |= (((res3 >> 16) & 0xFF) ^ 0x77) << 8;
        ret |= (((res3 >> 24) & 0xFF) ^ 0x61);
    } else {
        ret |= (((res3) & 0xFF) ^ 0x73) << 24;
        ret |= (((res3 >> 8) & 0xFF) ^ 0x65) << 16;
        ret |= (((res3 >> 16) & 0xFF) ^ 0x64) << 8;
        ret |= (((res3 >> 24) & 0xFF) ^ 0x6F);
    }
    return ret;
}

static bool hw_boot(const unsigned char *rom, unsigned size) {
    uint32_t sendsize = (size + 7) & ~7u;
    if (sendsize < 0x200) sendsize = 0x200;
    uint32_t raw;
    if (!hw_read(&raw)) return false;
    /* (the GBA's words arrive lowest byte first; the key arithmetic works on them byte-swapped) */
    uint32_t session = __builtin_bswap32(__builtin_bswap32(raw) ^ 0x7365646F);
    if (!hw_write(__builtin_bswap32(boot_key(sendsize)))) return false;
    uint32_t crc = 0x15A0;
    uint32_t i;
    for (i = 0; i < 0xC0; i += 4) {                    /* the header goes as it is */
        uint32_t v = rom[i] | ((uint32_t)rom[i + 1] << 8) | ((uint32_t)rom[i + 2] << 16) | ((uint32_t)rom[i + 3] << 24);
        if (!hw_write(v)) return false;
    }
    for (; i < sendsize; i += 4) {
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) if (i + k < size) v |= (uint32_t)rom[i + k] << (8 * k);
        crc = boot_crc(crc, v);
        session = session * 0x6177614B + 1;
        v ^= session;
        v ^= (~(i + (0x20 << 20))) + 1;
        v ^= 0x20796220;
        if (!hw_write(v)) return false;
    }
    crc |= sendsize << 16;
    session = session * 0x6177614B + 1;
    crc ^= session;
    crc ^= (~(i + (0x20 << 20))) + 1;
    crc ^= 0x20796220;
    if (!hw_write(crc)) return false;
    hw_read(&raw);   /* the GBA's checksum (not needed) */
    return true;
}

static void hw_init(void) { LWP_SemInit(&si_sem, 0, 1); }

#else   /* PC test build: the cable is simulated by the test program */

bool gbalink_hw_is_gba(void);
bool gbalink_hw_status(uint8_t *st);
bool gbalink_hw_reset(uint8_t *st);
bool gbalink_hw_read(uint32_t *w);
bool gbalink_hw_write(uint32_t w);
bool gbalink_hw_boot(const unsigned char *rom, unsigned size);
#define hw_is_gba gbalink_hw_is_gba
#define hw_status gbalink_hw_status
#define hw_reset gbalink_hw_reset
#define hw_read gbalink_hw_read
#define hw_write gbalink_hw_write
#define hw_boot gbalink_hw_boot
static void hw_init(void) {}
#endif

/* ============================================== shared by both threads */

enum { L_OFF, L_SEARCH, L_BOOTING, L_LINKED };
enum { EV_SENDING = 1, EV_LINKED = 2, EV_UNPLUGGED = 4, EV_NEED_EMPTY = 8, EV_BOOT_FAIL = 16, EV_NO_PROGRAM = 32 };

static thr_mutex lk;
static bool lk_ok;
static GbpTx tx;               /* to the GBA (filled by the main thread) */
static GbpRx rx;               /* from the GBA (read by the link thread) */
static volatile int link_st = L_OFF;
static volatile int events;
static volatile bool resend_all, resync, enabled;
static volatile int want_from = -1;   /* the GBA is missing songs from here */
#define CMDQ 32
static uint8_t cmdq[CMDQ][3];
static uint8_t seen_ids[16];    /* recent command ids, so one sent twice runs once */
static int seen_n;
static int cmd_head, cmd_tail;
static thr_t th;

static void post_event(int e) {
    thr_lock(&lk);
    events |= e;
    thr_unlock(&lk);
}

/* (link thread, lock held) a message from the GBA */
static void on_gba_message(void *ctx, int type, const uint8_t *d, int len) {
    (void)ctx;
    if (type == GBP_REQ_SYNC) {
        gbp_tx_reset(&tx);     /* start our stream afresh... */
        resync = true;         /* ...with the state in it (the song list only on request) */
    } else if (type == GBP_WANT && len >= 2) {
        want_from = (int)gbp_get16(d);
    } else if (type == GBP_CMD && len >= 4) {
        gbp_tx_put(&tx, GBP_ACK, d, 1);
        for (int i = 0; i < 16; i++) if (seen_ids[i] == d[0]) return;   /* already done */
        seen_ids[seen_n] = d[0];
        seen_n = (seen_n + 1) % 16;
        int n = (cmd_head + 1) % CMDQ;
        if (n != cmd_tail) { memcpy(cmdq[cmd_head], d + 1, 3); cmd_head = n; }
    }
}

static void start_session(void) {
    thr_lock(&lk);
    gbp_tx_reset(&tx);
    gbp_rx_reset(&rx);
    uint8_t v = GBP_VERSION;
    gbp_tx_put(&tx, GBP_HELLO, &v, 1);
    gbp_tx_ask_sync(&tx);   /* and ask the GBA to start its stream afresh */
    resend_all = true;
    link_st = L_LINKED;
    events |= EV_LINKED;
    thr_unlock(&lk);
}

static unsigned ms_now(void);

static void *link_main(void *arg) {
    (void)arg;
    bool told = false;
    int misses = 0;
    uint32_t pending = 0;              /* a word not yet delivered (sent again as it is) */
    unsigned last_req = 0;
    for (;;) {
        if (!enabled) { link_st = L_OFF; thr_sleep_ms(250); continue; }
        if (link_st != L_LINKED) {
            link_st = L_SEARCH;
            thr_sleep_ms(500);
            if (!hw_is_gba()) { told = false; continue; }
            uint8_t s = 0;
            if (!hw_status(&s)) continue;
            if ((s & 0x30) == GBP_STAT_PLAYER) {          /* the player is already running: carry on */
                pending = 0;
                misses = 0;
                start_session();
                continue;
            }
            if (!&gbaplayer_rom_size || !gbaplayer_rom_size) {
                if (!told) { post_event(EV_NO_PROGRAM); told = true; }
                continue;
            }
            /* waiting for a program? (reset and ask, for up to 1.5 s) */
            bool ready = false;
            for (int k = 0; k < 30 && !ready; k++) {
                hw_reset(&s);
                if (hw_status(&s) && (s & GBP_STAT_BIOS_READY)) ready = true;
                else thr_sleep_ms(50);
            }
            if (!ready) {   /* probably a game is running, or it's a GBA with a cartridge */
                if (!told) { post_event(EV_NEED_EMPTY); told = true; }
                thr_sleep_ms(2000);
                continue;
            }
            told = false;
            link_st = L_BOOTING;
            post_event(EV_SENDING);
            bool ok = hw_boot(gbaplayer_rom, gbaplayer_rom_size);
            /* wait for the player to start and say so */
            for (int k = 0; ok && k < 100; k++) {
                thr_sleep_ms(50);
                if (hw_status(&s) && (s & 0x30) == GBP_STAT_PLAYER) break;
                if (k == 99) ok = false;
            }
            if (!ok) { post_event(EV_BOOT_FAIL); link_st = L_SEARCH; thr_sleep_ms(1500); continue; }
            memset(seen_ids, 0, sizeof(seen_ids));   /* a fresh player numbers its commands from 1 */
            pending = 0;
            misses = 0;
            start_session();
            continue;
        }

        /* linked: one word each way */
        thr_lock(&lk);
        if (!pending) pending = gbp_tx_word(&tx);
        thr_unlock(&lk);
        if (pending) {
            if (hw_write(pending)) { pending = 0; misses = 0; }
            else misses++;
        }
        uint32_t w = 0;
        bool got = false;
        if (hw_read(&w)) {
            misses = 0;
            got = (w >> 24) != 0;
            thr_lock(&lk);
            bool ok = gbp_rx_word(&rx, w, on_gba_message, NULL);
            unsigned now = ms_now();
            if (!ok || (rx.lost && now - last_req > 500)) {   /* missed something: ask for a sync */
                gbp_tx_ask_sync(&tx);
                last_req = now;
            }
            thr_unlock(&lk);
        } else misses++;
        if (misses > 20) {   /* no answer for a while: unplugged or switched off */
            thr_lock(&lk);
            link_st = L_SEARCH;
            events |= EV_UNPLUGGED;
            thr_unlock(&lk);
            continue;
        }
        thr_lock(&lk);
        bool more = pending || gbp_tx_pending(&tx);
        thr_unlock(&lk);
        thr_sleep_ms(more || got ? 1 : 16);
    }
    return NULL;
}

/* ======================================================== main thread */

#ifdef GEKKO
#include <ogc/lwp_watchdog.h>
static unsigned ms_now(void) { return (unsigned)ticks_to_millisecs(gettime()); }
#else
#include <time.h>
static unsigned ms_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
#endif

/* what the GBA was last sent */
static uint8_t sent_state[GBP_STATE_BYTES];
static bool sent_state_ok;
static char sent_title[GBP_TEXT_MAX + 1], sent_instr[GBP_TEXT_MAX + 1], sent_msg[GBP_TEXT_MAX + 1];
static int songs_sent, songs_total, sfs_sent;
static unsigned list_hash, list_check_at, sf_scan_at;
static int save_in;            /* frames until the settings are saved (0 = nothing to save) */

/* the Instruments choices: automatic, built-in, then each .sf2 (as in Settings) */
static char sf_path[GBP_SF_MAX][256];
static uint16_t sf_size[GBP_SF_MAX];
static int sf_count;

static int cmp_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void scan_sf2(void) {
    sf_count = 0;
    snprintf(sf_path[sf_count], 256, "%s", "");
    sf_size[sf_count++] = 0;
    snprintf(sf_path[sf_count], 256, "%s", "builtin");
    sf_size[sf_count++] = 0;
    DIR *d = storage_ok() ? opendir(MIDI_DIR) : NULL;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && sf_count < GBP_SF_MAX) {
            size_t l = strlen(e->d_name);
            if (e->d_name[0] != '.' && l > 4 && !strcasecmp(e->d_name + l - 4, ".sf2") && l + sizeof(MIDI_DIR) + 1 < 256)
                snprintf(sf_path[sf_count++], 256, "%s/%.200s", MIDI_DIR, e->d_name);
        }
        closedir(d);
        qsort(sf_path + 2, (size_t)(sf_count - 2), sizeof(sf_path[0]), cmp_name);
        for (int i = 2; i < sf_count; i++) {
            struct stat stt;
            long long kb100 = stat(sf_path[i], &stt) == 0 ? (long long)stt.st_size / 102400 : 0;
            sf_size[i] = (uint16_t)(kb100 > 65535 ? 65535 : kb100 < 1 ? 1 : kb100);
        }
    }
}

static const char *sf_label(int i) {
    if (i == 0) return "Automatic";
    if (i == 1) return "Built-in";
    const char *b = strrchr(sf_path[i], '/');
    return b ? b + 1 : sf_path[i];
}

static int sf_current(void) {
    for (int i = 0; i < sf_count; i++) if (!strcmp(sf_path[i], g_set.music_sf2)) return i;
    return 0xFF;
}

/* a song's name as shown: its file name without the extension */
static void song_label(int i, char *out, size_t cap) {
    snprintf(out, cap, "%s", midi_file(i));
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

static unsigned songs_hash(int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++)
        for (const char *p = midi_file(i); *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return h;
}

static void run_command(const uint8_t *c) {
    int arg = c[1] | (c[2] << 8);
    switch (c[0]) {
    case GBC_TOGGLE: midi_toggle_pause(); break;
    case GBC_STOP: midi_stop(); break;
    case GBC_NEXT: midi_next(); break;
    case GBC_PREV: midi_prev(); break;
    case GBC_PLAY: if (arg < midi_count()) midi_play(arg); break;
    case GBC_SHUFFLE: midi_set_shuffle(arg != 0); save_in = 120; break;
    case GBC_REPEAT: midi_set_repeat(arg != 0); save_in = 120; break;
    case GBC_VOLUME:
        g_set.music_volume = arg > 10 ? 10 : arg;
        save_in = 120;
        break;
    case GBC_INSTR:
        if (arg < sf_count && strcmp(g_set.music_sf2, sf_path[arg])) {
            snprintf(g_set.music_sf2, sizeof(g_set.music_sf2), "%s", sf_path[arg]);
            midi_instruments_changed();
            save_in = 120;
        }
        break;
    case GBC_RESEND: resend_all = true; break;
    }
}

/* (lock held) sends what changed */
static void send_updates(void) {
    unsigned now = ms_now();
    if (resend_all) {
        resend_all = false;
        sent_state_ok = false;
        sent_title[0] = sent_instr[0] = sent_msg[0] = 1;   /* not a real text: forces a send */
        sent_title[1] = sent_instr[1] = sent_msg[1] = 0;
        songs_sent = sfs_sent = 0;
        if (midi_count() == 0) midi_scan();
        list_hash = songs_hash(midi_count());
        list_check_at = now + 2000;
        scan_sf2();
        sf_scan_at = now + 30000;
        resync = false;
        want_from = -1;
    }
    if (resync) {   /* the GBA lost track: the state, texts and instruments again */
        resync = false;
        sent_state_ok = false;
        sent_title[0] = sent_instr[0] = sent_msg[0] = 1;
        sent_title[1] = sent_instr[1] = sent_msg[1] = 0;
        sfs_sent = 0;
    }
    if (want_from >= 0) {
        if (want_from < songs_sent) songs_sent = want_from;
        want_from = -1;
    }
    int count = midi_count();
    if (count > 400) count = 400;
    /* the song list changed? (checked every 2 s) */
    if ((int)(now - list_check_at) >= 0) {
        unsigned h = songs_hash(count);
        if (h != list_hash || count != songs_total) { list_hash = h; songs_sent = 0; }
        list_check_at = now + 2000;
    }
    if ((int)(now - sf_scan_at) >= 0) {   /* new SoundFonts? (every 30 s) */
        int before = sf_count;
        scan_sf2();
        if (sf_count != before) sfs_sent = 0;
        sf_scan_at = now + 30000;
    }
    songs_total = count;

    GbpState s;
    MidiState ms = midi_state();
    s.state = ms == MIDI_LOADING ? GBS_LOADING : ms == MIDI_PLAYING ? GBS_PLAYING : ms == MIDI_PAUSED ? GBS_PAUSED : GBS_STOPPED;
    s.flags = (uint8_t)((midi_shuffle() ? GBF_SHUFFLE : 0) | (midi_repeat() ? GBF_REPEAT : 0));
    s.volume = (uint8_t)g_set.music_volume;
    s.sf_count = (uint8_t)sf_count;
    s.sf_cur = (uint8_t)sf_current();
    int cur = midi_current();
    s.cur = (uint16_t)(cur >= 0 && cur < count ? cur : 0xFFFF);
    s.count = (uint16_t)count;
    float pos = midi_position(), len = midi_length();
    s.pos = (uint16_t)(pos > 0 ? (pos > 65535 ? 65535 : pos) : 0);
    s.len = (uint16_t)(len > 0 ? (len > 65535 ? 65535 : len) : 0);
    uint8_t packed[GBP_STATE_BYTES];
    gbp_state_pack(&s, packed);
    if ((!sent_state_ok || memcmp(packed, sent_state, sizeof(packed))) && gbp_tx_free(&tx) > 40) {
        gbp_tx_put(&tx, GBP_STATE, packed, GBP_STATE_BYTES);
        memcpy(sent_state, packed, sizeof(packed));
        sent_state_ok = true;
    }

    struct { int type; const char *now; char *sent; } texts[3] = {
        { GBP_TITLE, midi_title(), sent_title }, { GBP_INSTR, midi_instruments(), sent_instr }, { GBP_MSG, midi_message(), sent_msg } };
    for (int i = 0; i < 3; i++) {
        char t[GBP_TEXT_MAX + 1];
        snprintf(t, sizeof(t), "%s", texts[i].now ? texts[i].now : "");
        if (strcmp(t, texts[i].sent) && gbp_tx_free(&tx) > 80) {
            gbp_tx_text(&tx, texts[i].type, NULL, 0, t);
            snprintf(texts[i].sent, GBP_TEXT_MAX + 1, "%s", t);
        }
    }

    /* the lists, a few at a time (the state goes first) */
    while (sfs_sent < sf_count && gbp_tx_free(&tx) > 128) {
        uint8_t pre[3] = { (uint8_t)sfs_sent, (uint8_t)sf_size[sfs_sent], (uint8_t)(sf_size[sfs_sent] >> 8) };
        gbp_tx_text(&tx, GBP_SF, pre, 3, sf_label(sfs_sent));
        sfs_sent++;
    }
    while (songs_sent < count && gbp_tx_free(&tx) > 256) {
        char name[GBP_TEXT_MAX + 1];
        song_label(songs_sent, name, sizeof(name));
        uint8_t pre[2];
        gbp_put16(pre, (unsigned)songs_sent);
        gbp_tx_text(&tx, GBP_SONG, pre, 2, name);
        songs_sent++;
    }
}

void gbalink_init(void) {
    lk_ok = thr_mutex_init(&lk);
    if (!lk_ok) return;
    hw_init();
    gbp_tx_reset(&tx);
    gbp_rx_reset(&rx);
    enabled = g_set.music_gba != 0;
    thr_create(&th, link_main, NULL, 32 * 1024, 70);
}

bool gbalink_linked(void) { return link_st == L_LINKED; }

void gbalink_frame(void) {
    if (!lk_ok) return;
    enabled = g_set.music_gba != 0;
    if (save_in > 0 && --save_in == 0) settings_save();

    uint8_t cmds[CMDQ][3];
    int ncmd = 0;
    thr_lock(&lk);
    int ev = events;
    events = 0;
    while (cmd_tail != cmd_head && ncmd < CMDQ) {
        memcpy(cmds[ncmd++], cmdq[cmd_tail], 3);
        cmd_tail = (cmd_tail + 1) % CMDQ;
    }
    thr_unlock(&lk);

    if (ev & EV_SENDING) ui_toast("Sending the music player to the Game Boy Advance...");
    if (ev & EV_LINKED) ui_toast("Game Boy Advance music player is ready");
    if (ev & EV_UNPLUGGED) ui_toast("Game Boy Advance unplugged");
    if (ev & EV_NEED_EMPTY) ui_toast("For the music player, turn the Game Boy Advance on with no game inserted");
    if (ev & EV_BOOT_FAIL) ui_toast("Couldn't send the music player to the Game Boy Advance. Trying again...");
    if (ev & EV_NO_PROGRAM) ui_toast("This build doesn't include the Game Boy Advance music player");
    for (int i = 0; i < ncmd; i++) run_command(cmds[i]);

    if (link_st != L_LINKED) return;
    thr_lock(&lk);
    send_updates();
    thr_unlock(&lk);
}
