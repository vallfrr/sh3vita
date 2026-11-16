#include "diag_taskmem.h"
#include <pthread.h>
#include <stdlib.h>
#define LIMIT 4096u
struct DiagTaskMem { DiagHeap *heap; pthread_mutex_t lock; uint32_t blocks[LIMIT]; unsigned count; };
DiagTaskMem *diag_taskmem_create(DiagHeap *heap) {
    if(!heap)return NULL;
    DiagTaskMem *m=calloc(1,sizeof *m);if(!m)return NULL;
    if(pthread_mutex_init(&m->lock,NULL)){free(m);return NULL;}
    m->heap=heap;return m;
}
uint32_t diag_taskmem_alloc(DiagTaskMem *m,uint32_t size) {
    if(!m || pthread_mutex_lock(&m->lock))return 0;
    uint32_t p=0;
    if(m->count<LIMIT && (p=diag_heap_alloc(m->heap,size,0)))m->blocks[m->count++]=p;
    pthread_mutex_unlock(&m->lock);return p;
}
int diag_taskmem_free(DiagTaskMem *m,uint32_t p) {
    if(!m)return -1;
    if(!p)return 0;
    if(pthread_mutex_lock(&m->lock))return -1;
    unsigned i=0;while(i<m->count && m->blocks[i]!=p)i++;
    int result=-1;
    if(i<m->count && !diag_heap_free(m->heap,p)) {m->blocks[i]=m->blocks[--m->count];m->blocks[m->count]=0;result=0;}
    pthread_mutex_unlock(&m->lock);return result;
}
int diag_taskmem_destroy(DiagTaskMem *m) {
    if(!m || pthread_mutex_lock(&m->lock))return -1;
    if(m->count){pthread_mutex_unlock(&m->lock);return -1;}
    pthread_mutex_unlock(&m->lock);pthread_mutex_destroy(&m->lock);free(m);return 0;
}
