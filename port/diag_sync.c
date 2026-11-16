/* Anonymous semaphore for the experimental host bridge. GPL-3.0. */
#include "diag_sync.h"
#include <SDL.h>
#include <stdlib.h>
struct DiagSem { SDL_mutex *lock; SDL_cond *cond; uint32_t count, maximum; };
DiagSem *diag_sem_create(uint32_t initial, uint32_t maximum) {
    if (!maximum || initial>maximum) return NULL;
    DiagSem *s=calloc(1,sizeof *s); if (!s) return NULL;
    s->lock=SDL_CreateMutex(); s->cond=SDL_CreateCond();
    if (!s->lock || !s->cond) { if(s->cond) SDL_DestroyCond(s->cond); if(s->lock) SDL_DestroyMutex(s->lock); free(s); return NULL; }
    s->count=initial; s->maximum=maximum; return s;
}
int diag_sem_wait(DiagSem *s,uint32_t ms) {
    if (!s || SDL_LockMutex(s->lock)) return -1;
    uint64_t deadline=ms==UINT32_MAX?0:SDL_GetTicks64()+(uint64_t)ms;
    int result=0;
    while (!s->count) {
        if (ms!=UINT32_MAX && SDL_GetTicks64()>=deadline) { result=258; break; }
        int r;
        if (ms==UINT32_MAX) r=SDL_CondWait(s->cond,s->lock);
        else {
            uint64_t now=SDL_GetTicks64();
            if (now>=deadline) { result=258; break; }
            uint64_t remain=deadline-now; r=SDL_CondWaitTimeout(s->cond,s->lock,(Uint32)remain);
        }
        if (r==SDL_MUTEX_TIMEDOUT && !s->count) { result=258; break; }
        if (r && r!=SDL_MUTEX_TIMEDOUT) { result=-1; break; }
    }
    if (!result) s->count--;
    if (SDL_UnlockMutex(s->lock)) return -1;
    return result;
}
int diag_sem_release(DiagSem *s,uint32_t n,uint32_t *previous) {
    if (!s || !n || SDL_LockMutex(s->lock)) return -1;
    if (n>s->maximum-s->count) { SDL_UnlockMutex(s->lock); return 1; }
    if (previous) *previous=s->count;
    s->count+=n;
    int r=SDL_CondBroadcast(s->cond);
    if (SDL_UnlockMutex(s->lock)) return -1;
    return r? -1:0;
}
void diag_sem_destroy(DiagSem *s) { if(!s)return; SDL_DestroyCond(s->cond); SDL_DestroyMutex(s->lock); free(s); }
