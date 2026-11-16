#pragma once
#include <stdint.h>
typedef struct DiagEvent DiagEvent;
/* SDL timer must be initialized; each caller owns a reference until destroy. */
DiagEvent *diag_event_create(int manual_reset,int initial_state);
int diag_event_set(DiagEvent *,int signaled);
/* Releases current waiters without retaining a signal; overlapping pulses are not queued. */
int diag_event_pulse(DiagEvent *);
int diag_event_wait(DiagEvent *,uint32_t timeout_ms); /* 0 signaled, 258 timeout, -1 error */
int diag_event_waiter_count(DiagEvent *);
void diag_event_destroy(DiagEvent *);
/* One table reference plus a reference per acquired operation. Closing removes
   the handle immediately, but outstanding wait/signal operations remain safe. */
uint32_t diag_event_handle_create(int manual_reset,int initial_state);
DiagEvent *diag_event_handle_acquire(uint32_t handle);
int diag_event_handle_close(uint32_t handle);
void diag_event_report(void);
unsigned diag_event_live_count(void);
