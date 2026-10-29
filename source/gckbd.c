/* gckbd.c - see gckbd.h
 *
 * The keyboard answers the poll command 0x54 with 8 bytes: a 4-bit counter
 * in the first byte, then (in the last four) up to three key codes and a
 * check byte, the XOR of the three keys and the counter. Key codes and the
 * protocol follow the Dolphin emulator's keyboard, which matches the real
 * one (so it can be tried there). */
#include "gckbd.h"
#include "settings.h"
#include <string.h>
#ifdef GEKKO
#include <gccore.h>
#include <ogc/si.h>
#endif

#define QMAX 24
static KbEvent queue[QMAX];
static int qn, qpos;
static bool connected;

static void push(KbKey k, char ch) {
    if (qn < QMAX) { queue[qn].key = k; queue[qn].ch = ch; qn++; }
}

bool kb_next(KbEvent *e) {
    if (qpos >= qn) return false;
    *e = queue[qpos++];
    return true;
}

void kb_clear(void) { qpos = qn; }
bool kb_connected(void) { return connected; }

/* --------------------------------------------------------- key codes */

enum {
    K_HOME = 0x06, K_END = 0x07, K_PGUP = 0x08, K_PGDN = 0x09,
    K_A = 0x10, K_Z = 0x29, K_1 = 0x2A, K_0 = 0x33,
    K_F5 = 0x44, K_ESC = 0x4C, K_DELETE = 0x4E, K_BACKSPACE = 0x50, K_TAB = 0x51, K_CAPS = 0x53,
    K_LSHIFT = 0x54, K_RSHIFT = 0x55, K_SPACE = 0x59,
    K_LEFT = 0x5C, K_DOWN = 0x5D, K_UP = 0x5E, K_RIGHT = 0x5F, K_ENTER = 0x61,
};

/* punctuation keys: code, character, with Shift */
static const struct { unsigned char code; char plain, shifted; } punct[] = {
    { 0x34, '-', '_' }, { 0x35, '=', '+' }, { 0x37, '[', '{' }, { 0x38, ']', '}' },
    { 0x39, ';', ':' }, { 0x3A, '\'', '"' }, { 0x3B, '\\', '|' }, { 0x3C, ',', '<' },
    { 0x3D, '.', '>' }, { 0x3E, '/', '?' }, { 0x3F, '\\', '_' }, { 0x4F, '`', '~' },
};

static bool caps_lock;

/* one key going down (or repeating) */
static void press(unsigned char code, bool shift) {
    if (code >= K_A && code <= K_Z) {
        bool upper = shift != caps_lock;
        push(KB_CHAR, (char)((upper ? 'A' : 'a') + (code - K_A)));
        return;
    }
    if (code >= K_1 && code <= K_0) {
        static const char digits[] = "1234567890", shifted[] = "!@#$%^&*()";
        push(KB_CHAR, (shift ? shifted : digits)[code - K_1]);
        return;
    }
    for (unsigned i = 0; i < sizeof(punct) / sizeof(punct[0]); i++)
        if (punct[i].code == code) { push(KB_CHAR, shift ? punct[i].shifted : punct[i].plain); return; }
    switch (code) {
    case K_SPACE: push(KB_CHAR, ' '); break;
    case K_BACKSPACE: push(KB_BACKSPACE, 0); break;
    case K_DELETE: push(KB_DELETE, 0); break;
    case K_ENTER: push(KB_ENTER, 0); break;
    case K_ESC: push(KB_ESC, 0); break;
    case K_TAB: push(KB_TAB, 0); break;
    case K_LEFT: push(KB_LEFT, 0); break;
    case K_RIGHT: push(KB_RIGHT, 0); break;
    case K_UP: push(KB_UP, 0); break;
    case K_DOWN: push(KB_DOWN, 0); break;
    case K_HOME: push(KB_HOME, 0); break;
    case K_END: push(KB_END, 0); break;
    case K_PGUP: push(KB_PGUP, 0); break;
    case K_PGDN: push(KB_PGDN, 0); break;
    case K_F5: push(KB_F5, 0); break;
    case K_CAPS: caps_lock = !caps_lock; break;
    }
}

/* the three keys held now: new ones are presses; the newest one held repeats */
static unsigned char held[3];
static int repeat_frames;
static unsigned char repeat_code;

static void keys_now(const unsigned char k[3]) {
    bool shift = false;
    for (int i = 0; i < 3; i++) if (k[i] == K_LSHIFT || k[i] == K_RSHIFT) shift = true;
    for (int i = 0; i < 3; i++) {
        unsigned char c = k[i];
        if (!c || c == K_LSHIFT || c == K_RSHIFT) continue;
        bool was = false;
        for (int j = 0; j < 3; j++) if (held[j] == c) was = true;
        if (!was) { press(c, shift); repeat_code = c; repeat_frames = 0; }
    }
    bool still = false;
    for (int i = 0; i < 3; i++) if (repeat_code && k[i] == repeat_code) still = true;
    if (!still) repeat_code = 0;
    else if (++repeat_frames >= 30 && (repeat_frames - 30) % 3 == 0 && repeat_code != K_CAPS) press(repeat_code, shift);
    memcpy(held, k, 3);
}

/* ------------------------------------------------------ the controller port */

#ifdef GEKKO
static int chan = -1;          /* the port the keyboard is on */
static int look_timer;
static int bad_sums;           /* check bytes that didn't match, in a row */

static void stop(void) {
    if (chan >= 0) SI_DisablePolling(0x80000000u >> chan);
    chan = -1;
    connected = false;
    memset(held, 0, sizeof(held));
    repeat_code = 0;
}

void kb_update(void) {
    qn = qpos = 0;
    if (!g_set.kb_enabled) { if (chan >= 0) stop(); return; }
    if (chan < 0) {
        /* look for a keyboard about twice a second (the type is kept by the SI library) */
        if (++look_timer < 30) return;
        look_timer = 0;
        for (int c = 0; c < 4; c++) {
            if (SI_Probe(c) == SI_GC_KEYBOARD) {
                chan = c;
                bad_sums = 0;
                SI_SetCommand(c, 0x00540000);   /* the keyboard's poll command */
                SI_EnablePolling(0x80000000u >> c);
                connected = true;
                break;
            }
        }
        return;
    }
    u32 type = SI_Probe(chan);   /* unplugged, or swapped for something else? (busy now and then is normal) */
    if (type == SI_ERROR_NO_RESPONSE || (type != SI_GC_KEYBOARD && type != SI_ERROR_BUSY && type != SI_ERROR_UNKNOWN)) { stop(); return; }
    u32 buf[2];
    if (!SI_GetResponse(chan, buf)) return;   /* nothing new this frame */
    if (buf[0] & 0x80000000u) return;         /* a transfer error */
    unsigned char k[3] = { (unsigned char)(buf[1] >> 24), (unsigned char)(buf[1] >> 16), (unsigned char)(buf[1] >> 8) };
    unsigned char sum = (unsigned char)buf[1], counter = (unsigned char)((buf[0] >> 24) & 0x0f);
    if ((unsigned char)(k[0] ^ k[1] ^ k[2] ^ counter) != sum) {
        /* a garbled reading; but if every reading disagrees, the check
         * itself must differ on this keyboard, so stop relying on it */
        if (++bad_sums < 120) return;
    } else {
        bad_sums = 0;
    }
    keys_now(k);
}
#else
/* PC tests: keys come from kb_pc_type, one per frame */
static char pc_text[256];
static int pc_pos;

void kb_pc_type(const char *s) {
    strncpy(pc_text, s, sizeof(pc_text) - 1);
    pc_pos = 0;
    connected = true;
}

void kb_update(void) {
    qn = qpos = 0;
    if (!g_set.kb_enabled) return;
    unsigned char k[3] = { 0, 0, 0 };
    char c = pc_text[pc_pos];
    if (c) {
        pc_pos++;
        if (c == '\b') k[0] = K_BACKSPACE;
        else if (c == '\n') k[0] = K_ENTER;
        else if (c == ' ') k[0] = K_SPACE;
        else if (c >= 'a' && c <= 'z') k[0] = (unsigned char)(K_A + c - 'a');
        else if (c >= 'A' && c <= 'Z') { k[0] = K_LSHIFT; k[1] = (unsigned char)(K_A + c - 'A'); }
        else if (c >= '1' && c <= '9') k[0] = (unsigned char)(K_1 + c - '1');
        else if (c == '0') k[0] = K_0;
        else for (unsigned i = 0; i < sizeof(punct) / sizeof(punct[0]); i++) if (punct[i].plain == c) k[0] = punct[i].code;
    }
    memset(held, 0, sizeof(held));   /* every key is a fresh press */
    keys_now(k);
}
#endif
