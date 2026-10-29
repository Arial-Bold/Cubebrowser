/* gbaproto.h - how CubeBrowser and its Game Boy Advance music player talk
 * over the GameCube's link cable. Shared by both programs (gbalink.c on the
 * GameCube, gba/ on the Game Boy Advance).
 *
 * The link moves one 32-bit word at a time in each direction. Each word
 * carries a sequence number and three bytes of a byte stream:
 *
 *   bits 31..24  sequence: 1..253 in turn, 0 = nothing to send,
 *                0xFF = "sync": start reading afresh from the next word,
 *                0xFE = "please send me a sync word" (understood even by a
 *                receiver that has lost its place)
 *   bits 23..0   three bytes of the stream (lowest byte first)
 *
 * The stream is a series of messages: [type][length][length bytes].
 * Type 0 is padding and is skipped. A receiver that sees a sequence number
 * out of turn has lost a word: it ignores everything until the next sync
 * word, and asks the other side for one (a 0xFE word). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GBP_VERSION 1

/* GameCube -> GBA */
enum {
    GBP_HELLO = 1,      /* u8 version */
    GBP_STATE,          /* see GbpState */
    GBP_TITLE,          /* text: the song playing (or selected) */
    GBP_INSTR,          /* text: the instruments in use */
    GBP_SONG,           /* u16 index, text: one song of the list */
    GBP_SF,             /* u8 index, u16 size in 100 KB units, text: one Instruments choice */
    GBP_MSG,            /* text: a problem to show, or empty */
    GBP_ACK,            /* u8 id: that command arrived */
    GBP_THEME,          /* GBT_COUNT colours (u16 each, the GBA's 15-bit format): the browser's theme,
                         * then a style byte (1: System's square look) */
};
/* both ways: passed to the handler when the other side asks for a sync word */
#define GBP_REQ_SYNC 0x30
/* GBA -> GameCube */
#define GBP_CMD 0x40        /* u8 id (1..255), u8 command, u16 argument; sent again until acknowledged */
#define GBP_WANT 0x41       /* u16 index: songs from here on didn't arrive, please send them again */

enum {
    GBC_TOGGLE = 1,     /* play / pause (starts the first song when stopped) */
    GBC_STOP,
    GBC_NEXT,
    GBC_PREV,
    GBC_PLAY,           /* arg: song index */
    GBC_SHUFFLE,        /* arg: 0 off, 1 on */
    GBC_REPEAT,         /* arg: 0 off, 1 on */
    GBC_VOLUME,         /* arg: 0..10 */
    GBC_INSTR,          /* arg: Instruments choice index */
    GBC_RESEND,         /* send the lists again */
};

/* The colours the player draws with (GBP_THEME), worked out from the
 * browser's theme so the GBA matches the TV. The ...ON_BG / ..._TXT ones are
 * text colours already checked to be readable on what they're drawn over. */
enum {
    GBT_BG0, GBT_BG1,           /* background gradient, top to bottom */
    GBT_BAR, GBT_BARL,          /* tab bar and boxes, and their outline */
    GBT_EDGE, GBT_ACC,          /* light outline, highlight */
    GBT_TXT, GBT_DIM, GBT_ONACC,/* text on boxes and rows, softer text, text on the highlight */
    GBT_FIELD, GBT_FTXT, GBT_FDIM,   /* the white "now playing" card and its text */
    GBT_ROW, GBT_ROWL, GBT_BTN, GBT_TRACK, GBT_VOLOFF,
    GBT_HINT, GBT_GOOD, GBT_BAD, GBT_DECO,
    GBT_BGTXT, GBT_BGDIM, GBT_BGDIM2, GBT_BGACC,   /* text drawn straight on the background */
    GBT_HINTTXT, GBT_BARTXT,    /* text on the hint bar, and on the tab bar */
    GBT_CUBE1, GBT_CUBE2,       /* the cube's lit and shaded faces */
    GBT_COUNT
};

/* player states, as in midi.h */
enum { GBS_STOPPED, GBS_LOADING, GBS_PLAYING, GBS_PAUSED };

#define GBF_SHUFFLE 1
#define GBF_REPEAT  2

typedef struct {
    uint8_t  state, flags, volume, sf_cur, sf_count;
    uint16_t cur, count;       /* song index (0xFFFF none), number of songs */
    uint16_t pos, len;         /* seconds */
} GbpState;
#define GBP_STATE_BYTES 13

#define GBP_TEXT_MAX 48        /* longest text sent (bytes) */
#define GBP_SF_MAX 16          /* Instruments choices sent */

/* JOY BUS status bits (JOYSTAT on the GBA) used to tell what's running:
 * the GBA's start-up program sets 0x10 when it's ready to receive a program;
 * the music player sets 0x20. */
#define GBP_STAT_BIOS_READY 0x10
#define GBP_STAT_PLAYER     0x20

/* ---- sending ---- */
#define GBP_TX_BYTES 2048
typedef struct {
    uint8_t buf[GBP_TX_BYTES];
    uint16_t head, tail;       /* bytes [tail, head) are waiting */
    uint8_t seq;               /* last sequence number used */
    bool sync_due;             /* next word is a sync word */
    bool ask_due;              /* send a "please sync" word */
} GbpTx;

void gbp_tx_reset(GbpTx *t);                    /* drop everything, send a sync word next */
int  gbp_tx_free(const GbpTx *t);               /* bytes of room */
bool gbp_tx_put(GbpTx *t, int type, const void *data, int len);   /* false if no room */
bool gbp_tx_text(GbpTx *t, int type, const uint8_t *prefix, int plen, const char *text);
bool gbp_tx_pending(const GbpTx *t);
void gbp_tx_ask_sync(GbpTx *t);                 /* ask the other side for a sync word */
uint32_t gbp_tx_word(GbpTx *t);                 /* next word to send (0 = nothing) */

/* ---- receiving ---- */
typedef void (*GbpHandler)(void *ctx, int type, const uint8_t *data, int len);
typedef struct {
    uint8_t seq;               /* last sequence number seen */
    bool lost;                 /* waiting for a sync word */
    uint8_t type, len, got, stage;
    uint8_t msg[256];
} GbpRx;

void gbp_rx_reset(GbpRx *r);                    /* lost until the next sync word */
/* Feeds one received word. Returns false if a word was missed (the receiver
 * is now lost and should ask for a sync). */
bool gbp_rx_word(GbpRx *r, uint32_t w, GbpHandler h, void *ctx);

/* little-endian helpers */
static inline void gbp_put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline unsigned gbp_get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
void gbp_state_pack(const GbpState *s, uint8_t *out);
void gbp_state_unpack(GbpState *s, const uint8_t *in);
