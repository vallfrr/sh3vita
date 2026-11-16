#pragma once
#include "diag_heap.h"
#include <stdint.h>
typedef struct DiagTaskMem DiagTaskMem;
DiagTaskMem *diag_taskmem_create(DiagHeap *heap);
uint32_t diag_taskmem_alloc(DiagTaskMem *,uint32_t size);
int diag_taskmem_free(DiagTaskMem *,uint32_t ptr); /* zero is accepted */
int diag_taskmem_destroy(DiagTaskMem *); /* refuses live allocations */
