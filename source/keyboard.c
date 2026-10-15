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

#ifdef GEKKO
#include <gccore.h>
#include <ogc/si.h>

#define SI_KEYBOARD_ID   0x08200000u
#define SI_KEYBOARD_POLL 0x54000000u

static int frames, misses;

static bool is_keyboard(u32 type) {
    return (type & 0xFFFF0000u) == SI_KEYBOARD_ID;
}

static void start_polling(int chan) {
    SI_SetCommand(chan, SI_KEYBOARD_POLL);
    SI_EnablePolling((u32)(0x80000000u >> chan));
    SI_TransferCommands();
    port = chan;
    misses = 0;
    memset(held, 0, sizeof(held));
    kbd_clear();
}

void kbd_update(void) {
    frames++;
    if (port < 0) {
        if (frames % 60) return;   /* look for a keyboard once a second */
        for (int c = 0; c < 4; c++) {
            u32 t = SI_GetType(c);
            if (!(t & 0x80) && is_keyboard(t)) { start_polling(c); return; }   /* 0x80 = busy */
        }
        return;
    }
    u32 resp[2] = { 0, 0 };
    if (SI_GetResponse(port, resp)) {
        misses = 0;
        unsigned char now[3] = { (unsigned char)(resp[1] >> 24), (unsigned char)(resp[1] >> 16),
                                 (unsigned char)(resp[1] >> 8) };
        unsigned char sum = (unsigned char)(now[0] ^ now[1] ^ now[2] ^ (resp[0] >> 24));
        if (sum == (unsigned char)resp[1]) process(now);   /* ignore garbled reads */
    } else if (++misses > 120) {
        /* nothing for two seconds: check it's still there */
        u32 t = SI_GetType(port);
        if (!(t & 0x80) && !is_keyboard(t)) {
            SI_DisablePolling((u32)(0x80000000u >> port));
            port = -1;
            memset(held, 0, sizeof(held));
        }
        misses = 0;
    }
}
#else
void kbd_update(void) {}
#endif
