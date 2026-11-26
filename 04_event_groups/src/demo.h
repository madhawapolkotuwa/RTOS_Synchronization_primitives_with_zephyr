/*
 * The interface between main.c (hardware, heartbeat, reporting) and the
 * demo that is compiled in: startup_gate.c (Part A) or dispatcher.c
 * (Part B). Exactly one of them is built, selected in prj.conf.
 */
#ifndef DEMO_H_
#define DEMO_H_

#include <stdbool.h>

/* Implemented by the demo. */
void demo_start(void);           /* once, from main(), after the GPIO setup */
void demo_button_pressed(void);  /* from the button ISR (debounced): interrupt context */
void demo_report(void);          /* every CONFIG_APP_REPORT_PERIOD_MS, from main() */

/* Implemented by main.c. Both are safe to call from an ISR. */
void event_led_toggle(void);
void event_led_set(bool on);

#endif /* DEMO_H_ */
