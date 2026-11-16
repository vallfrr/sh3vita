#pragma once
#include "diag_event.h"
#include <stdint.h>
/* Experimental wait-any for retained event objects only; pulses may be missed. */
int diag_multiwait_events(DiagEvent *const *events,unsigned count,uint32_t timeout_ms);
