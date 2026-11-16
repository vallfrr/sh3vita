/* Bounded, thread-safe diagnostic FIFO; does not implement all Win32 queues. */
#include "diag_message.h"
#include <pthread.h>
#include <stdlib.h>
_Static_assert(sizeof(DiagMessage)==28,"diagnostic MSG layout must be 28 bytes");
struct DiagMessageQueue { pthread_mutex_t lock; DiagMessage *items; unsigned capacity,head,count; };
DiagMessageQueue *diag_message_create(unsigned capacity) {
    if (!capacity || capacity>4096) return NULL;
    DiagMessageQueue *q=calloc(1,sizeof *q); if(!q) return NULL;
    q->items=calloc(capacity,sizeof *q->items);
    if(!q->items) { free(q); return NULL; }
    if(pthread_mutex_init(&q->lock,NULL)) { free(q->items); free(q); return NULL; }
    q->capacity=capacity; return q;
}
void diag_message_destroy(DiagMessageQueue *q) {
    if(!q)return; pthread_mutex_destroy(&q->lock); free(q->items); free(q);
}
int diag_message_post(DiagMessageQueue *q,const DiagMessage *m) {
    if(!q || !m || pthread_mutex_lock(&q->lock)) return -1;
    int result=1;
    if(q->count<q->capacity) { q->items[(q->head+q->count)%q->capacity]=*m; q->count++; result=0; }
    if(pthread_mutex_unlock(&q->lock)) return -1;
    return result;
}
int diag_message_peek(DiagMessageQueue *q,DiagMessage *m,int remove) {
    if(!q || !m || (remove!=0 && remove!=1) || pthread_mutex_lock(&q->lock)) return -1;
    int result=0;
    if(q->count) { *m=q->items[q->head]; result=1; if(remove) { q->head=(q->head+1)%q->capacity; q->count--; } }
    if(pthread_mutex_unlock(&q->lock)) return -1;
    return result;
}
