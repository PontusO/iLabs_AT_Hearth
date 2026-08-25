/*
 * mt_cmdbox.h - the verdict mailbox: a single-slot state machine that hands
 * a forwarded Matter command a sequence number, waits for the host's
 * verdict, and lets the caller collect it exactly once.
 *
 * Deliberately free of IDF/FreeRTOS dependencies so the state machine is
 * unit-testable on the host (test/host). The blocking wait, the semaphore,
 * and the URC formatting all live in mt_at.c; this file only tracks the
 * slot's state and the pending verdict. One command can be in flight at a
 * time, mirroring the hardware: the CHIP event loop that would open a
 * second one is the same task blocked waiting on the first.
 *
 * Sequence numbers are a u32, start at 1, and increase on every
 * mt_cmdbox_open() regardless of how the previous one ended. 0 is never
 * issued; it is reserved as "no command open" so a caller cannot mistake an
 * uninitialised seq variable for a real one. Wrap is theoretical at this
 * command rate, but mt_cmdbox_open() skips back over 0 if it ever happens.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MT_CMDBOX_IDLE,
    MT_CMDBOX_PENDING,
    MT_CMDBOX_ANSWERED,
} mt_cmdbox_state_t;

/* Reset the mailbox to IDLE and the sequence counter to its first value (1).
 * Call once at boot; also used by the host tests to isolate each case. */
void mt_cmdbox_init(void);

/*
 * Open a new pending command: ep/cluster/command identify it for whatever
 * the caller does with them (mt_at.c uses them to format the +MTCMD URC),
 * the mailbox itself only needs a fresh seq to hand back. Always succeeds:
 * the single slot is reused, so a second open() while one is already
 * PENDING or ANSWERED simply discards whatever was there. Returns the new
 * sequence number.
 */
uint32_t mt_cmdbox_open(uint16_t ep, uint32_t cluster, uint32_t command);

/*
 * Record the host's verdict for a PENDING command. verdict must be 0
 * (deny) or 1 (allow); seq must match the slot currently PENDING.
 * Returns 0 and moves the slot to ANSWERED on success; returns -1 and
 * changes nothing for a wrong/stale/future seq, a slot not PENDING, or an
 * out-of-range verdict. seq 0 is always rejected, structurally rather than
 * by the seq-mismatch check alone: it is reserved for the +MTCMD
 * notify-only wire form (no mailbox slot is ever opened for it), so
 * AT+MTCMDRESP=0,... must answer +MTERR:1 unconditionally.
 */
int mt_cmdbox_answer(uint32_t seq, int verdict);

/*
 * Collect the verdict for an ANSWERED command and return the slot to IDLE.
 * Returns the stored verdict (0 or 1) on success; returns -1 if the slot is
 * not ANSWERED for this seq, including a second take() of the same answer.
 */
int mt_cmdbox_take(uint32_t seq);

/*
 * Drop a PENDING command that timed out: PENDING -> IDLE for a matching
 * seq, otherwise a no-op. A late AT+MTCMDRESP for this seq then finds the
 * slot not PENDING and mt_cmdbox_answer() rejects it with -1.
 */
void mt_cmdbox_expire(uint32_t seq);

#ifdef __cplusplus
}
#endif
