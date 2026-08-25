/*
 * main.c - nRF54L15 skeleton entry point. Boot contract per the layout
 * spec section 5: bring up the (stubbed) Matter runtime, then
 * mt_at_start(), which emits +MTREADY.
 */

#include "mt_at.h"

int main(void)
{
    mt_at_start();
    return 0;
}
