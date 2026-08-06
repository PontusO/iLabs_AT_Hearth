/*
 * mt_cmdbox.c - the verdict mailbox slot state machine.
 *
 * Deliberately free of IDF/FreeRTOS dependencies so the state machine is
 * unit-testable on the host (test/host). The critical sections that make
 * this safe against concurrent access from the AT parser task and the CHIP
 * event loop task live in mt_at.c, around the calls into here, not inside
 * this file: this file just holds one struct's worth of state and does not
 * know it is shared.
 */

#include "mt_cmdbox.h"

typedef struct {
    mt_cmdbox_state_t state;
    uint32_t           seq;
    int                verdict;
    uint16_t           ep;
    uint32_t           cluster;
    uint32_t           command;
} mt_cmdbox_slot_t;

static mt_cmdbox_slot_t s_slot;
static uint32_t         s_next_seq;

void mt_cmdbox_init(void)
{
    s_slot.state   = MT_CMDBOX_IDLE;
    s_slot.seq     = 0;
    s_slot.verdict = -1;
    s_slot.ep      = 0;
    s_slot.cluster = 0;
    s_slot.command = 0;
    s_next_seq     = 1;
}

uint32_t mt_cmdbox_open(uint16_t ep, uint32_t cluster, uint32_t command)
{
    s_slot.ep      = ep;
    s_slot.cluster = cluster;
    s_slot.command = command;
    s_slot.seq     = s_next_seq;
    s_slot.state   = MT_CMDBOX_PENDING;
    s_slot.verdict = -1;

    s_next_seq++;
    if (s_next_seq == 0) {
        /* Skip back over the reserved idle marker on the (theoretical)
         * wrap. */
        s_next_seq = 1;
    }

    return s_slot.seq;
}

int mt_cmdbox_answer(uint32_t seq, int verdict)
{
    if (verdict != 0 && verdict != 1) {
        return -1;
    }
    if (s_slot.state != MT_CMDBOX_PENDING || s_slot.seq != seq) {
        return -1;
    }

    s_slot.verdict = verdict;
    s_slot.state   = MT_CMDBOX_ANSWERED;
    return 0;
}

int mt_cmdbox_take(uint32_t seq)
{
    if (s_slot.state != MT_CMDBOX_ANSWERED || s_slot.seq != seq) {
        return -1;
    }

    int verdict = s_slot.verdict;
    s_slot.state   = MT_CMDBOX_IDLE;
    s_slot.seq     = 0;
    s_slot.verdict = -1;
    return verdict;
}

void mt_cmdbox_expire(uint32_t seq)
{
    if (s_slot.state == MT_CMDBOX_PENDING && s_slot.seq == seq) {
        s_slot.state = MT_CMDBOX_IDLE;
        s_slot.seq   = 0;
    }
}
