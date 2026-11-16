/* Single guest-thread lifecycle primitive for experimental host use. GPL-3.0. */
#include "diag_thread.h"
#include <SDL.h>
#include <stdlib.h>
struct DiagThread { SDL_mutex *lock; SDL_cond *cond; SDL_Thread *thread; SDL_threadID owner; DiagThreadEntry entry; void *arg; uint32_t result; int done,joined,suspended; };
static int thread_entry(void *opaque) {
    DiagThread *t=opaque;
    SDL_LockMutex(t->lock);
    t->owner=SDL_ThreadID();
    while(t->suspended) SDL_CondWait(t->cond,t->lock);
    SDL_UnlockMutex(t->lock);
    uint32_t r=t->entry(t->arg);
    SDL_LockMutex(t->lock); t->result=r; t->done=1; SDL_CondBroadcast(t->cond); SDL_UnlockMutex(t->lock);
    return 0;
}
static DiagThread *create(DiagThreadEntry entry,void *arg,int suspended) {
    if (!entry) return NULL;
    DiagThread *t=calloc(1,sizeof *t); if (!t) return NULL;
    t->entry=entry; t->arg=arg; t->suspended=suspended; t->lock=SDL_CreateMutex(); t->cond=SDL_CreateCond();
    if (!t->lock || !t->cond) goto fail;
    t->thread=SDL_CreateThread(thread_entry,"sh3-guest",t); if (!t->thread) goto fail;
    return t;
fail: if(t->cond)SDL_DestroyCond(t->cond); if(t->lock)SDL_DestroyMutex(t->lock); free(t); return NULL;
}
DiagThread *diag_thread_create(DiagThreadEntry entry,void *arg) { return create(entry,arg,0); }
DiagThread *diag_thread_create_suspended(DiagThreadEntry entry,void *arg) { return create(entry,arg,1); }
int diag_thread_resume(DiagThread *t) {
    if (!t || SDL_LockMutex(t->lock)) return -1;
    int was=t->suspended;
    if (was) { t->suspended=0; if (SDL_CondBroadcast(t->cond)) { SDL_UnlockMutex(t->lock); return -1; } }
    if (SDL_UnlockMutex(t->lock)) return -1;
    return was;
}
int diag_thread_self_suspend(DiagThread *t) {
    if (!t || SDL_LockMutex(t->lock)) return -1;
    if (t->owner!=SDL_ThreadID() || t->suspended || t->done) { SDL_UnlockMutex(t->lock); return -1; }
    t->suspended=1;
    while(t->suspended) if (SDL_CondWait(t->cond,t->lock)) { SDL_UnlockMutex(t->lock); return -1; }
    if (SDL_UnlockMutex(t->lock)) return -1;
    return 0;
}
int diag_thread_is_suspended(DiagThread *t) {
    if (!t || SDL_LockMutex(t->lock)) return -1;
    int suspended=t->suspended;
    if (SDL_UnlockMutex(t->lock)) return -1;
    return suspended;
}
int diag_thread_wait(DiagThread *t,uint32_t ms,uint32_t *result) {
    if (!t || SDL_LockMutex(t->lock)) return -1;
    uint64_t deadline=ms==UINT32_MAX?0:SDL_GetTicks64()+(uint64_t)ms; int r=0;
    while (!t->done) {
        if (ms!=UINT32_MAX && SDL_GetTicks64()>=deadline) { r=258; break; }
        int w;
        if (ms==UINT32_MAX) w=SDL_CondWait(t->cond,t->lock);
        else { uint64_t now=SDL_GetTicks64(); if(now>=deadline){r=258;break;} w=SDL_CondWaitTimeout(t->cond,t->lock,(Uint32)(deadline-now)); }
        if (w==SDL_MUTEX_TIMEDOUT && !t->done) { r=258; break; }
        if (w && w!=SDL_MUTEX_TIMEDOUT) { r=-1; break; }
    }
    if (!r && result) *result=t->result;
    if (SDL_UnlockMutex(t->lock)) return -1;
    return r;
}
int diag_thread_join(DiagThread *t) {
    if (!t || SDL_LockMutex(t->lock)) return -1;
    if (t->joined || !t->done) { SDL_UnlockMutex(t->lock); return -1; }
    t->joined=1; SDL_Thread *thread=t->thread;
    SDL_UnlockMutex(t->lock); SDL_WaitThread(thread,NULL); t->thread=NULL; return 0;
}
void diag_thread_destroy(DiagThread *t) {
    if (!t) return;
    if (!t->joined) abort();
    SDL_DestroyCond(t->cond); SDL_DestroyMutex(t->lock); free(t);
}
