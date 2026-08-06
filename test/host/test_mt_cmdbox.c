/*
 * Host unit tests for the verdict mailbox state machine. No framework: the
 * project has no test dependency and this needs none. Build and run with:
 *     make -C test/host run
 */

#include <stdbool.h>
#include <stdio.h>

#include "mt_cmdbox.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
    }
}

static void test_open_monotonic(void)
{
    mt_cmdbox_init();
    uint32_t s1 = mt_cmdbox_open(1, 0x0006, 0x02);
    uint32_t s2 = mt_cmdbox_open(1, 0x0006, 0x02);
    uint32_t s3 = mt_cmdbox_open(2, 0x0101, 0x00);
    check("first seq is 1 (0 is reserved as the idle marker)", s1 == 1);
    check("seqs increase monotonically across opens", s2 == s1 + 1 && s3 == s2 + 1);
}

static void test_answer_then_take_once(void)
{
    mt_cmdbox_init();
    uint32_t seq = mt_cmdbox_open(1, 0x0006, 0x02);
    check("answer with the pending seq and a legal verdict is accepted",
          mt_cmdbox_answer(seq, 1) == 0);
    check("take returns the stored verdict", mt_cmdbox_take(seq) == 1);
    check("a second take of the same seq returns -1 (slot is IDLE again)",
          mt_cmdbox_take(seq) == -1);
}

static void test_answer_wrong_seq_rejected(void)
{
    mt_cmdbox_init();
    uint32_t seq = mt_cmdbox_open(1, 0x0006, 0x02);
    check("answering seq 0 (stale/never issued) is rejected", mt_cmdbox_answer(0, 1) == -1);
    check("answering a future seq is rejected", mt_cmdbox_answer(seq + 1, 1) == -1);
    check("the pending slot is untouched: it still answers to its own seq",
          mt_cmdbox_answer(seq, 1) == 0);
}

static void test_answer_in_idle_rejected(void)
{
    mt_cmdbox_init();
    check("answering with nothing ever opened is rejected", mt_cmdbox_answer(1, 1) == -1);
}

static void test_answer_bad_verdict_rejected(void)
{
    mt_cmdbox_init();
    uint32_t seq = mt_cmdbox_open(1, 0x0006, 0x02);
    check("verdict 2 is rejected", mt_cmdbox_answer(seq, 2) == -1);
    check("verdict -1 is rejected", mt_cmdbox_answer(seq, -1) == -1);
    check("the slot is still PENDING afterwards and answers normally",
          mt_cmdbox_answer(seq, 0) == 0);
}

static void test_expire_drops_pending(void)
{
    mt_cmdbox_init();
    uint32_t seq = mt_cmdbox_open(1, 0x0006, 0x02);
    mt_cmdbox_expire(seq);
    check("a late answer for an expired seq is rejected", mt_cmdbox_answer(seq, 1) == -1);
}

static void test_reopen_after_expire_fresh_seq(void)
{
    mt_cmdbox_init();
    uint32_t seq1 = mt_cmdbox_open(1, 0x0006, 0x02);
    mt_cmdbox_expire(seq1);
    uint32_t seq2 = mt_cmdbox_open(1, 0x0006, 0x02);
    check("reopening after an expire issues a fresh seq", seq2 == seq1 + 1);
    check("the fresh seq answers normally", mt_cmdbox_answer(seq2, 1) == 0);
    check("take returns the verdict for the fresh seq", mt_cmdbox_take(seq2) == 1);
}

/*
 * Final review of the command-forwarding round, fix wave. mt_cmdbox_open()'s
 * own header comment documents that a second open() while the single slot
 * is already PENDING or ANSWERED "simply discards whatever was there" --
 * no existing test above actually pinned that claim: every case up to here
 * opens exactly once per mt_cmdbox_init(). Pinned here directly, both
 * discard paths, since PENDING (never answered) and ANSWERED (answered but
 * not yet taken) are reached through different call sequences and both
 * need their own late-arrival check.
 */
static void test_reopen_discards_pending_or_answered_slot(void)
{
    mt_cmdbox_init();
    uint32_t seq1 = mt_cmdbox_open(1, 0x0006, 0x02); /* PENDING, never answered */
    uint32_t seq2 = mt_cmdbox_open(2, 0x0101, 0x00); /* a second forward opens over it */
    check("reopening over a still-PENDING slot issues a fresh seq", seq2 == seq1 + 1);
    check("a late answer for the discarded PENDING seq is rejected",
          mt_cmdbox_answer(seq1, 1) == -1);
    check("the fresh seq answers normally", mt_cmdbox_answer(seq2, 1) == 0);

    mt_cmdbox_init();
    uint32_t seq3 = mt_cmdbox_open(1, 0x0006, 0x02);
    check("seq3 answers, leaving the slot ANSWERED but not yet taken",
          mt_cmdbox_answer(seq3, 1) == 0);
    uint32_t seq4 = mt_cmdbox_open(2, 0x0101, 0x00); /* a second forward opens over the un-taken answer */
    check("reopening over an un-taken ANSWERED slot issues a fresh seq", seq4 == seq3 + 1);
    check("a late answer for the discarded ANSWERED seq is rejected",
          mt_cmdbox_answer(seq3, 1) == -1);
    check("a take() of the discarded ANSWERED seq is rejected too",
          mt_cmdbox_take(seq3) == -1);
    check("the fresh seq answers and takes normally",
          mt_cmdbox_answer(seq4, 0) == 0 && mt_cmdbox_take(seq4) == 0);
}

int main(void)
{
    printf("\n===== mt_cmdbox tests =====\n");
    test_open_monotonic();
    test_answer_then_take_once();
    test_answer_wrong_seq_rejected();
    test_answer_in_idle_rejected();
    test_answer_bad_verdict_rejected();
    test_expire_drops_pending();
    test_reopen_after_expire_fresh_seq();
    test_reopen_discards_pending_or_answered_slot();
    printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
