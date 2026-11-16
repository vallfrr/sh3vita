#pragma once
#include <stdint.h>
typedef struct DiagThread DiagThread;
typedef uint32_t (*DiagThreadEntry)(void *);
/* The caller owns the argument until join. Closing is allowed only after join. */
DiagThread *diag_thread_create(DiagThreadEntry entry, void *argument);
DiagThread *diag_thread_create_suspended(DiagThreadEntry entry, void *argument);
/* Return 1 if resumed, 0 if already running, -1 on error. */
int diag_thread_resume(DiagThread *);
/* Only the calling worker can suspend itself. Returns 0 after resume, -1 on error. */
int diag_thread_self_suspend(DiagThread *);
int diag_thread_is_suspended(DiagThread *);
/* 0=completed, 258=timeout, -1=error; output is written only on completion. */
int diag_thread_wait(DiagThread *,uint32_t timeout_ms,uint32_t *result);
int diag_thread_join(DiagThread *);
void diag_thread_destroy(DiagThread *);
