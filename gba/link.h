/* link.h - the GBA side of the link cable conversation (gbaproto.h). The
 * interrupt handler passes words in and takes words out; the main loop calls
 * link_frame once per frame. No hardware here, so it can be tested on a PC. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

void     link_init(void);
uint32_t link_first_word(void);          /* put in JOY_TRANS at start */
void     link_irq_received(uint32_t w);  /* (interrupt) the GameCube wrote a word */
uint32_t link_irq_next(void);            /* (interrupt) the GameCube read ours: the next one */
int      link_frame(void);               /* (main loop) handles what came in; returns LINK_* */
void     link_send_cmd(int cmd, int arg);
