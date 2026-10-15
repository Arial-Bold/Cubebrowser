/* keyboard.c - reads the GameCube ASCII Keyboard Controller over the serial
 * interface (SI). libogc has no keyboard support, so this talks to the port
 * directly:
 *   - a keyboard identifies itself with device type 0x0820xxxx
 *   - polling it with command 0x54 returns 8 bytes: a counter in the first
 *     byte, then up to three key codes currently held, then a checksum
 *     (the key codes and counter XORed together)
 * Key codes follow the keyboard's own numbering (A = 0x10 ... Z = 0x29,
 * 1..0 = 0x2A..0x33, and so on). */
#include "keyboard.h"
#include "settings.h"
#include "gc_entropy.h"
#include <string.h>

#define QUEUE 32

static KeyEvent queue[QUEUE];
static int qhead, qtail;
static int port = -1;
static int last_code;
static unsigned char held[3];
static int repeat_code, repeat_timer;
static bool caps;
static unsigned port_types[4];
static bool port_seen[4];

static void push(const KeyEvent *ev) {
    int next = (qtail + 1) % QUEUE;
    if (next == qhead) return;   /* full: drop */
    queue[qtail] = *ev;
    qtail = next;
}

bool kbd_next(KeyEvent *ev) {
    if (qhead == qtail) return false;
    *ev = queue[qhead];
    qhead = (qhead + 1) % QUEUE;
    return true;
}

void kbd_clear(void) { qhead = qtail = 0; }
bool kbd_connected(void) { return port >= 0; }
int kbd_port(void) { return port + 1; }
int kbd_last_code(void) { return last_code; }

/* Characters by key position. JIS (Japanese) is what the original keyboard
 * prints; US matches the positions of a US keyboard, which is what most
 * Bluetooth keyboards paired through BlueRetro will expect. */
int kbd_translate(int code, bool shift, bool jis) {
    if (code >= 0x10 && code <= 0x29) return (shift ? 'A' : 'a') + (code - 0x10);
    if (code >= 0x2A && code <= 0x33) {
        static const char us_s[] = "!@#$%^&*()", jis_s[] = "!\"#$%&'()0";
        int i = code - 0x2A;
        if (!shift) return i == 9 ? '0' : '1' + i;
        return jis ? jis_s[i] : us_s[i];
    }
    switch (code) {
    case 0x06: return KEY_HOME;
    case 0x07: return KEY_END;
    case 0x08: return KEY_PGUP;
    case 0x09: return KEY_PGDN;
    case 0x34: return jis ? (shift ? '=' : '-') : (shift ? '_' : '-');
    case 0x35: return jis ? (shift ? '~' : '^') : (shift ? '+' : '=');
    case 0x36: return jis ? (shift ? '`' : '@') : (shift ? '{' : '[');
    case 0x37: return jis ? (shift ? '{' : '[') : (shift ? '}' : ']');
    case 0x38: return jis ? (shift ? '+' : ';') : (shift ? ':' : ';');
    case 0x39: return jis ? (shift ? '*' : ':') : (shift ? '"' : '\'');
    case 0x3A: return jis ? (shift ? '}' : ']') : (shift ? '|' : '\\');
    case 0x3B: return shift ? '<' : ',';
    case 0x3C: return shift ? '>' : '.';
    case 0x3D: return shift ? '?' : '/';
    case 0x3E: return jis ? (shift ? '_' : '\\') : (shift ? '|' : '\\');
    case 0x4C: return KEY_ESC;
    case 0x4D: return KEY_INSERT;
    case 0x4E: return KEY_DELETE;
    case 0x4F: return jis ? KEY_NONE : (shift ? '~' : '`');
    case 0x50: return KEY_BACKSPACE;
    case 0x51: return KEY_TAB;
    case 0x59: return ' ';
    case 0x5C: return KEY_LEFT;
    case 0x5D: return KEY_DOWN;
    case 0x5E: return KEY_UP;
    case 0x5F: return KEY_RIGHT;
    case 0x61: return KEY_ENTER;
    }
    if (code >= 0x40 && code <= 0x4B) return KEY_F1 + (code - 0x40);
    return KEY_NONE;
}

static bool is_modifier(int code) {
    return code == 0x53 || code == 0x54 || code == 0x55 || code == 0x56 || code == 0x57 || code == 0x58 ||
           code == 0x5A || code == 0x5B;
}

/* Turns the set of held keys into press events (with auto-repeat). */
static __attribute__((unused)) void process(const unsigned char now[3]) {
    bool shift = false, ctrl = false, alt = false;
    for (int i = 0; i < 3; i++) {
        if (now[i] == 0x54 || now[i] == 0x55) shift = true;
        if (now[i] == 0x56) ctrl = true;
        if (now[i] == 0x57) alt = true;
    }
    for (int i = 0; i < 3; i++) {
        int code = now[i];
        if (!code) continue;
        bool was_held = false;
        for (int k = 0; k < 3; k++) if (held[k] == code) was_held = true;
        if (was_held) continue;
        last_code = code;
        entropy_add_event(0x6B000000u | (unsigned)code);
        if (code == 0x53) { caps = !caps; continue; }
        if (is_modifier(code)) continue;
        bool sh = shift;
        if (caps && code >= 0x10 && code <= 0x29) sh = !sh;
        KeyEvent ev = { kbd_translate(code, sh, g_set.kbd_layout == 1), ctrl, shift, alt };
        if (ev.key != KEY_NONE) push(&ev);
        repeat_code = code;
        repeat_timer = 0;
    }
    /* auto-repeat the most recent non-modifier key while it stays held */
    bool still = false;
    for (int k = 0; k < 3; k++) if (now[k] && now[k] == repeat_code) still = true;
    if (!still) repeat_code = 0;
    else if (++repeat_timer > 28 && (repeat_timer - 28) % (12 - g_set.key_repeat) == 0) {
        bool sh = shift;
        if (caps && repeat_code >= 0x10 && repeat_code <= 0x29) sh = !sh;
        KeyEvent ev = { kbd_translate(repeat_code, sh, g_set.kbd_layout == 1), ctrl, shift, alt };
        if (ev.key != KEY_NONE) push(&ev);
    }
    memcpy(held, now, 3);
}

#include <stdio.h>

void kbd_describe_ports(char *out, int n) {
    int o = 0;
    out[0] = 0;
    for (int c = 0; c < 4 && o < n - 1; c++) {
        const char *what = "empty";
        unsigned t = port_types[c];
        if (!port_seen[c]) what = "not checked yet";
        else if ((t & 0xFFFF0000u) == 0x08200000u) what = "keyboard";
        else if (t & 0x08000000u) what = (t & 0x01000000u) ? "controller" : "other device";
        o += snprintf(out + o, (size_t)(n - o), "%sPort %d: %s (%08X)", c ? "   " : "", c + 1, what, t);
    }
}

#ifdef GEKKO
#include <gccore.h>
#include <ogc/si.h>


/* Device identity, asked directly. SI_GetType() can't be used here: once a
 * port is being polled it keeps returning the type it first saw, so a port
 * that started out as a controller is never reported as a keyboard, even
 * after BlueRetro switches it. Sending the identity command (0x00) ourselves
 * always gets the device's current answer: two ID bytes and a status byte. */
static u8 id_cmd __attribute__((aligned(32))) = 0x00;
static u8 id_resp[4][32] __attribute__((aligned(32)));
static volatile bool id_ready[4], id_failed[4];
static int frames, misses;

static void id_callback(s32 chan, u32 status) {
    if (chan < 0 || chan > 3) return;
    if (status & 0x0F) id_failed[chan] = true;   /* no response / collision / overrun */
    else id_ready[chan] = true;
}

static void ask_identity(int c) {
    id_ready[c] = id_failed[c] = false;
    memset(id_resp[c], 0, 4);
    SI_Transfer(c, &id_cmd, 1, id_resp[c], 3, id_callback, 0);
}

/* Reads back the answers to the last identity requests. */
static void collect_identities(void) {
    for (int c = 0; c < 4; c++) {
        if (id_ready[c]) {
            port_types[c] = ((unsigned)id_resp[c][0] << 24) | ((unsigned)id_resp[c][1] << 16) | ((unsigned)id_resp[c][2] << 8);
            port_seen[c] = true;
            id_ready[c] = false;
        } else if (id_failed[c]) {
            port_types[c] = 0;   /* nothing answered: empty port */
            port_seen[c] = true;
            id_failed[c] = false;
        }
    }
}

static bool is_keyboard(unsigned type) { return (type & 0xFFFF0000u) == 0x08200000u; }

/* Reading keys: once a keyboard is found, its port is taken away from the
 * controller library (which would otherwise keep reading the port as a
 * controller and consume the keyboard's data) and the keyboard is asked for
 * its keys directly every frame with the read command 0x54. The 8-byte reply
 * is: counter, 3 unused bytes, 3 key codes, checksum. */
static u8 read_cmd[32] __attribute__((aligned(32))) = { 0x54, 0x00, 0x00 };
static u8 read_resp[32] __attribute__((aligned(32)));
static volatile int read_state;          /* 0 idle, 1 waiting, 2 got data, 3 failed */
static int claim_frames;                 /* frames to wait after handing the port over */
static unsigned stat_ok, stat_badsum, stat_fail;
static unsigned last_hi, last_lo;
static unsigned char pending[3];
static bool have_pending;
static int last_data_frame, wait_frames;
static bool checking_port;   /* identity check of the keyboard's own port in progress */

static void read_callback(s32 chan, u32 status) {
    (void)chan;
    read_state = (status & 0x0F) ? 3 : 2;
}

static void start_reading(int chan) {
    PAD_Reset((u32)(0x80000000u >> chan));          /* controller library lets go of the port */
    SI_DisablePolling((u32)(0x80000000u >> chan));  /* and stops its controller polling */
    port = chan;
    claim_frames = 10;
    read_state = 0;
    last_data_frame = frames;
    checking_port = false;
    misses = 0;
    have_pending = false;
    memset(held, 0, sizeof(held));
    kbd_clear();
}

static void stop_reading(void) {
    port = -1;
    read_state = 0;
    checking_port = false;
    memset(held, 0, sizeof(held));
    kbd_clear();
}

void kbd_stats(char *out, int n) {
    snprintf(out, (size_t)n, "Reports %u   checksum mismatches %u   failed reads %u   last %08X %08X",
             stat_ok, stat_badsum, stat_fail, last_hi, last_lo);
}

void kbd_update(void) {
    frames++;
    collect_identities();
    if (port < 0) {
        if (frames % 120 == 45) for (int c = 0; c < 4; c++) ask_identity(c);
        if (frames % 120 == 75)
            for (int c = 0; c < 4; c++)
                if (port_seen[c] && is_keyboard(port_types[c])) { start_reading(c); break; }
        return;
    }
    if (claim_frames > 0) { claim_frames--; return; }

    /* the other ports are still checked every two seconds */
    if (frames % 120 == 45)
        for (int c = 0; c < 4; c++) if (c != port) ask_identity(c);

    /* Is the keyboard still there? Ask its port too every two seconds (between
     * reads), and treat two seconds without any report as a disconnection,
     * whatever the reason (failed, unfinished or missing replies). */
    if (checking_port) {
        if (port_seen[port]) {
            checking_port = false;
            if (!is_keyboard(port_types[port])) { stop_reading(); return; }   /* now a controller, or nothing */
        } else if (frames % 120 >= 40) {
            checking_port = false;   /* no clear answer: carry on reading; silence still counts */
            port_seen[port] = true;
        }
    } else if (frames % 120 == 30 && read_state == 0) {
        port_seen[port] = false;
        ask_identity(port);
        checking_port = true;
        return;   /* skip this frame's read so the two requests don't collide */
    }
    if (frames - last_data_frame > 120) { stop_reading(); return; }

    if (read_state == 2) {
        u32 hi = ((u32)read_resp[0] << 24) | ((u32)read_resp[1] << 16) | ((u32)read_resp[2] << 8) | read_resp[3];
        u32 lo = ((u32)read_resp[4] << 24) | ((u32)read_resp[5] << 16) | ((u32)read_resp[6] << 8) | read_resp[7];
        last_hi = hi;
        last_lo = lo;
        last_data_frame = frames;
        unsigned char now[3] = { read_resp[4], read_resp[5], read_resp[6] };
        unsigned char sum = (unsigned char)(now[0] ^ now[1] ^ now[2] ^ read_resp[0]);
        if (sum == read_resp[7]) {
            stat_ok++;
            have_pending = false;
            process(now);
        } else {
            /* Checksum differs (some adapters compute it differently): accept
             * the keys once two reports in a row agree, which still filters
             * out a one-off garbled read. */
            stat_badsum++;
            if (have_pending && !memcmp(pending, now, 3)) { have_pending = false; process(now); }
            else { memcpy(pending, now, 3); have_pending = true; }
        }
        read_state = 0;
    } else if (read_state == 3) {
        stat_fail++;
        read_state = 0;
    } else if (read_state == 1 && ++wait_frames > 30) {
        stat_fail++;          /* a request that never finished: give up on it */
        read_state = 0;
    }
    if (read_state == 0 && !checking_port) {
        read_state = 1;
        wait_frames = 0;
        memset(read_resp, 0, 8);
        if (!SI_Transfer(port, read_cmd, 3, read_resp, 8, read_callback, 0)) read_state = 0;   /* busy: retry next frame */
    }
}

#else
void kbd_update(void) {}
void kbd_stats(char *out, int n) { snprintf(out, (size_t)n, "(not on a GameCube)"); }
#endif
