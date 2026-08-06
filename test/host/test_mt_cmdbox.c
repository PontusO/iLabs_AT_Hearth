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
    printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
