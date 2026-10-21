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
#include <string.h>
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
    memcpy(VRAM16, back, sizeof(back));

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
                if (hold[i] > 18 && (hold[i] - 18) % 5 == 0) repeat |= (u16)(1 << i);
            } else hold[i] = 0;
        }

        player_link(link_frame());
        player_input(pressed, repeat);
        player_frame();
        if (player_needs_draw()) {
            player_draw();
            memcpy(VRAM16, back, sizeof(back));   /* a plain copy, so link interrupts keep running */
        }
    }
}
