/* hearth_console.h - the console's one declaration, so check_decls.py sees
 * it and a signature drift between main.cpp and hearth_port_sl.c is a
 * compile error, not undefined behaviour. TX only on USART0 PA00; there is
 * no console input path on this platform (board contract item 6). */
#ifndef HEARTH_CONSOLE_H
#define HEARTH_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up USART0 TX on PA00 at 115200 8N1. Call once, after the SDK's
 * device init has brought the clocks up and before the first log line
 * that must be seen. Everything logged before it returns is dropped. */
void hearth_console_init(void);

#ifdef __cplusplus
}
#endif

#endif
