#pragma once
#include <stdint.h>
typedef struct DiagSem DiagSem;
/* SDL timer must be initialized. No destruction while waiters or users exist. */
DiagSem *diag_sem_create(uint32_t initial, uint32_t maximum);
/* Returns 0 on acquisition, 258 on timeout, -1 on internal failure. */
int diag_sem_wait(DiagSem *sem, uint32_t timeout_ms);
/* Returns 0 on success, 1 on overflow, -1 on invalid count or internal failure. */
int diag_sem_release(DiagSem *sem, uint32_t count, uint32_t *previous);
void diag_sem_destroy(DiagSem *sem);
