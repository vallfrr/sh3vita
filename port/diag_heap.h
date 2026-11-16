#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct DiagHeap DiagHeap;
/* The arena must remain alive, and must not overlap the bookkeeping object. */
DiagHeap *diag_heap_create(uint8_t *arena,size_t arena_size,uint32_t base,uint32_t length);
uint32_t diag_heap_alloc(DiagHeap *heap,uint32_t size,int zero);
int diag_heap_free(DiagHeap *heap,uint32_t address);
int diag_heap_size(DiagHeap *heap,uint32_t address,uint32_t *size);
/* Read-only inventory, serialized with allocation/free. */
void diag_heap_report(DiagHeap *heap);
/* Caller must ensure no concurrent users and no live allocations. */
int diag_heap_destroy(DiagHeap *heap);
