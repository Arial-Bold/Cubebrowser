/* main.c - CubeBrowser's music player for the Game Boy Advance.
 *
 * The GameCube sends this program over the link cable (no cartridge needed)
 * and then talks to it through the cable's JOY BUS registers: the GameCube
 * writes one 32-bit word at a time into JOY_RECV and reads ours from
 * JOY_TRANS. Each finished write or read raises an interrupt here, which
 * hands the word received to link.c or puts the next one in place.
 * Everything else (decoding, the screens, buttons) happens in the main loop:
 * link.c and player.c. */
#include <gba_base.h>
#include <gba_interrupt.h>
#include <gba_systemcalls.h>
#include "player.h"
#include "link.h"
#include "gfx.h"
#include "gbaproto.h"

#define REG_DISPCNT_  (*(vu16 *)0x04000000)
#define REG_KEYS      (*(vu16 *)0x04000130)
#define REG_RCNT_     (*(vu16 *)0x04000134)
#define REG_JOYCNT    (*(vu16 *)0x04000140)
#define REG_JOY_RECV  (*(vu32 *)0x04000150)
#define REG_JOY_TRANS (*(vu32 *)0x04000154)
#define REG_JOYSTAT   (*(vu16 *)0x04000158)
#define VRAM16        ((u16 *)0x06000000)
#define REG_DMA3SAD   (*(vu32 *)0x040000D4)
#define REG_DMA3DAD   (*(vu32 *)0x040000D8)
#define REG_DMA3CNT   (*(vu32 *)0x040000DC)

#define JOY_RESET 1       /* JOYCNT: the GameCube sent "reset" */
#define JOY_RECV  2       /* JOYCNT: the GameCube wrote a word */
#define JOY_SENT  4       /* JOYCNT: the GameCube read our word */
#define JOY_IRQ   0x40

static EWRAM_BSS u16 back[SCR_W * SCR_H];

static void joy_irq(void) {
    u16 c = REG_JOYCNT;
    if (c & JOY_RECV) link_irq_received(REG_JOY_RECV);
    if (c & JOY_SENT) REG_JOY_TRANS = link_irq_next();
    REG_JOYCNT = (u16)((c & (JOY_RESET | JOY_RECV | JOY_SENT)) | JOY_IRQ);   /* acknowledge */
}

/* Copies the finished picture to the screen with the DMA hardware, 8 rows at
 * a time: much quicker than a copy loop, and the CPU (and so the link
 * interrupt) only waits about half a millisecond for each piece. */
static void present(void) {
    const u32 words = SCR_W * 8 / 2;
    for (int y = 0; y < SCR_H; y += 8) {
        REG_DMA3SAD = (u32)(back + y * SCR_W);
        REG_DMA3DAD = (u32)(VRAM16 + y * SCR_W);
        REG_DMA3CNT = words | 0x84000000u;   /* start now, 32-bit units */
    }
}

void player_send(int cmd, int arg) { link_send_cmd(cmd, arg); }

void player_screen_power(bool on) {
    if (on) REG_DISPCNT_ &= (u16)~0x80;
    else REG_DISPCNT_ |= 0x80;            /* blank: saves a little power */
}

int main(void) {
    irqInit();
    irqSet(IRQ_SERIAL, joy_irq);
    irqEnable(IRQ_VBLANK | IRQ_SERIAL);

    REG_DISPCNT_ = 3 | (1 << 10);          /* 240x160 bitmap, 15-bit colour */
    fb = back;
    player_init();
    player_draw();
    present();

    /* the link: JOY BUS mode, "player running" in the status bits, interrupts on */
    link_init();
    REG_RCNT_ = 0xC000;
    REG_JOYSTAT = GBP_STAT_PLAYER;
    REG_JOY_TRANS = link_first_word();
    REG_JOYCNT = JOY_RESET | JOY_RECV | JOY_SENT | JOY_IRQ;

    u16 prev = 0, hold[8] = { 0 };
    for (;;) {
        VBlankIntrWait();

        /* buttons, with auto-repeat on the D-pad */
        u16 keys = (u16)(~REG_KEYS & 0x3FF);
        u16 pressed = keys & (u16)~prev, repeat = 0;
        prev = keys;
        for (int i = 4; i < 8; i++) {
            if (keys & (1 << i)) {
                hold[i]++;
                if (hold[i] > 14 && (hold[i] - 14) % 4 == 0) repeat |= (u16)(1 << i);
            } else hold[i] = 0;
        }

        player_link(link_frame());
        player_input(pressed, repeat);
        player_frame();
        if (player_needs_draw()) {
            player_draw();
            present();
        }
    }
}
