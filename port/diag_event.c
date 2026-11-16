/* Windows-like unnamed manual/auto reset event for the diagnostic. GPL-3.0. */
#include "diag_event.h"
#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
struct DiagEvent { SDL_mutex *lock; SDL_cond *cond; int manual,state; unsigned waiters,pulse_generation,pulse_remaining,references; };
static unsigned created,destroyed,peak,live_count;
#define EVENT_SLOTS 4096u
static struct { uint32_t handle; DiagEvent *event; } handles[EVENT_SLOTS];
static uint32_t next_handle;
static pthread_mutex_t table_lock=PTHREAD_MUTEX_INITIALIZER;
DiagEvent *diag_event_create(int manual,int state) {
    DiagEvent *e=calloc(1,sizeof *e); if (!e) return NULL;
    e->lock=SDL_CreateMutex(); e->cond=SDL_CreateCond();
    if (!e->lock || !e->cond) { fprintf(stderr,"[event] SDL creation failed: %s\n",SDL_GetError()); if(e->cond)SDL_DestroyCond(e->cond); if(e->lock)SDL_DestroyMutex(e->lock); free(e); return NULL; }
    e->manual=!!manual; e->state=!!state;e->references=1;
    __atomic_add_fetch(&created,1,__ATOMIC_RELAXED);
    unsigned live=__atomic_add_fetch(&live_count,1,__ATOMIC_RELAXED);
    unsigned old=__atomic_load_n(&peak,__ATOMIC_RELAXED);
    while(live>old && !__atomic_compare_exchange_n(&peak,&old,live,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
    return e;
}
int diag_event_set(DiagEvent *e,int signaled) {
    if (!e || SDL_LockMutex(e->lock)) return -1;
    e->state=!!signaled; int r=0;
    if (e->state) r=e->manual?SDL_CondBroadcast(e->cond):SDL_CondSignal(e->cond);
    if (SDL_UnlockMutex(e->lock)) return -1;
    return r? -1:0;
}
int diag_event_pulse(DiagEvent *e) {
    if (!e || SDL_LockMutex(e->lock)) return -1;
    e->state=0; e->pulse_generation++;
    e->pulse_remaining=e->manual?e->waiters:(e->waiters?1u:0u);
    int r=e->pulse_remaining?SDL_CondBroadcast(e->cond):0;
    if (SDL_UnlockMutex(e->lock)) return -1;
    return r?-1:0;
}
int diag_event_waiter_count(DiagEvent *e) {
    if (!e || SDL_LockMutex(e->lock)) return -1;
    int n=(int)e->waiters;
    if (SDL_UnlockMutex(e->lock)) return -1;
    return n;
}
int diag_event_wait(DiagEvent *e,uint32_t ms) {
    if (!e || SDL_LockMutex(e->lock)) return -1;
    uint64_t deadline=ms==UINT32_MAX?0:SDL_GetTicks64()+(uint64_t)ms; int r=0;
    unsigned observed=e->pulse_generation;
    while(!e->state && !(e->pulse_generation!=observed && e->pulse_remaining)) {
        if (ms!=UINT32_MAX && SDL_GetTicks64()>=deadline) { r=258; break; }
        int w;
        e->waiters++;
        if(ms==UINT32_MAX) w=SDL_CondWait(e->cond,e->lock);
        else { uint64_t now=SDL_GetTicks64(); if(now>=deadline){e->waiters--;r=258;break;} w=SDL_CondWaitTimeout(e->cond,e->lock,(Uint32)(deadline-now)); }
        e->waiters--;
        if(w==SDL_MUTEX_TIMEDOUT && !e->state && !(e->pulse_generation!=observed && e->pulse_remaining)) { r=258; break; }
        if(w && w!=SDL_MUTEX_TIMEDOUT) { r=-1; break; }
    }
    if(!r) {
        if(e->state) { if(!e->manual)e->state=0; }
        else if(e->pulse_remaining) e->pulse_remaining--;
    }
    if(SDL_UnlockMutex(e->lock)) return -1;
    return r;
}
void diag_event_destroy(DiagEvent *e) {
    if(!e || __atomic_sub_fetch(&e->references,1,__ATOMIC_ACQ_REL))return;
    SDL_DestroyCond(e->cond);SDL_DestroyMutex(e->lock);free(e);
    __atomic_add_fetch(&destroyed,1,__ATOMIC_RELAXED);
    __atomic_sub_fetch(&live_count,1,__ATOMIC_RELAXED);
}
uint32_t diag_event_handle_create(int manual,int state) {
    DiagEvent *event=diag_event_create(manual,state);if(!event)return 0;
    pthread_mutex_lock(&table_lock);
    uint32_t result=0;
    /* Generation is part of the synthetic handle: a closed stale handle must
       never resolve to a different event when its table slot is reused. */
    for(unsigned n=0;n<EVENT_SLOTS && next_handle<=0xffffffu;n++,next_handle++) {
        unsigned slot=next_handle&(EVENT_SLOTS-1u);
        if(handles[slot].event)continue;
        result=0xf9000000u|next_handle++;
        handles[slot].handle=result;handles[slot].event=event;break;
    }
    pthread_mutex_unlock(&table_lock);
    if(!result)diag_event_destroy(event);
    return result;
}
DiagEvent *diag_event_handle_acquire(uint32_t handle) {
    if((handle&0xff000000u)!=0xf9000000u)return NULL;
    pthread_mutex_lock(&table_lock);
    unsigned slot=handle&(EVENT_SLOTS-1u);
    DiagEvent *event=handles[slot].handle==handle?handles[slot].event:NULL;
    if(event)__atomic_add_fetch(&event->references,1,__ATOMIC_RELAXED);
    pthread_mutex_unlock(&table_lock);return event;
}
int diag_event_handle_close(uint32_t handle) {
    if((handle&0xff000000u)!=0xf9000000u)return -1;
    pthread_mutex_lock(&table_lock);
    unsigned slot=handle&(EVENT_SLOTS-1u);
    DiagEvent *event=handles[slot].handle==handle?handles[slot].event:NULL;
    if(event)handles[slot].event=NULL;
    pthread_mutex_unlock(&table_lock);
    if(!event)return -1;
    diag_event_destroy(event);return 0;
}
void diag_event_report(void) {
    unsigned c=__atomic_load_n(&created,__ATOMIC_RELAXED),d=__atomic_load_n(&destroyed,__ATOMIC_RELAXED);
    fprintf(stderr,"[event-live] created=%u destroyed=%u live=%u peak=%u\n",c,d,diag_event_live_count(),__atomic_load_n(&peak,__ATOMIC_RELAXED));
}
unsigned diag_event_live_count(void) {return __atomic_load_n(&live_count,__ATOMIC_RELAXED);}
