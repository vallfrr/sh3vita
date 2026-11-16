#include "diag_event.h"
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
static int signaler(void *p) { SDL_Delay(20); return diag_event_set(p,1); }
static int waiter(void *p) { return diag_event_wait(p,1000); }
static int owned_waiter(void *p) {int r=diag_event_wait(p,1000);diag_event_destroy(p);return r;}
static int handle_stress(void *unused) {
    (void)unused;
    for(unsigned i=0;i<6000;i++) {
        uint32_t h=diag_event_handle_create(0,0);assert(h);
        DiagEvent *e=diag_event_handle_acquire(h);assert(e);
        assert(!diag_event_handle_close(h));assert(!diag_event_handle_acquire(h));
        assert(diag_event_handle_close(h)==-1);
        assert(!diag_event_set(e,1));assert(!diag_event_wait(e,0));diag_event_destroy(e);
    }
    return 0;
}
int main(void) {
    assert(!SDL_Init(SDL_INIT_TIMER));
    DiagEvent *a=diag_event_create(0,0),*m=diag_event_create(1,1); assert(a && m);
    assert(diag_event_wait(a,0)==258); assert(!diag_event_wait(m,0)); assert(!diag_event_wait(m,0));
    assert(!diag_event_set(m,0)); assert(diag_event_wait(m,0)==258);
    SDL_Thread *t=SDL_CreateThread(signaler,"event-test",a); assert(t);
    assert(!diag_event_wait(a,1000)); int r=-1; SDL_WaitThread(t,&r); assert(!r);
    assert(diag_event_wait(a,0)==258); assert(!diag_event_set(a,1)); assert(!diag_event_wait(a,0)); assert(diag_event_wait(a,0)==258);
    assert(!diag_event_pulse(a)); assert(diag_event_wait(a,0)==258);
    t=SDL_CreateThread(waiter,"pulse-test",a); assert(t);
    uint64_t deadline=SDL_GetTicks64()+1000;
    while(diag_event_waiter_count(a)!=1 && SDL_GetTicks64()<deadline) SDL_Delay(1);
    assert(diag_event_waiter_count(a)==1); assert(!diag_event_pulse(a)); SDL_WaitThread(t,&r); assert(!r);
    assert(diag_event_wait(a,0)==258);
    diag_event_destroy(a); diag_event_destroy(m);assert(!diag_event_live_count());
    /* A closed handle cannot be acquired again, but a previously acquired
       waiter is still alive until its own reference is released. */
    uint32_t h=diag_event_handle_create(0,0);assert(h);
    DiagEvent *held=diag_event_handle_acquire(h),*wait=diag_event_handle_acquire(h);assert(held && wait);
    t=SDL_CreateThread(owned_waiter,"close-waiter",wait);assert(t);
    deadline=SDL_GetTicks64()+1000;
    while(diag_event_waiter_count(held)!=1 && SDL_GetTicks64()<deadline)SDL_Delay(1);
    assert(diag_event_waiter_count(held)==1);assert(!diag_event_handle_close(h));
    assert(!diag_event_handle_acquire(h));assert(diag_event_live_count()==1);
    assert(!diag_event_set(held,1));diag_event_destroy(held);SDL_WaitThread(t,&r);assert(!r);
    assert(!diag_event_live_count());
    SDL_Thread *workers[4];
    for(unsigned i=0;i<4;i++){workers[i]=SDL_CreateThread(handle_stress,"event-reuse",NULL);assert(workers[i]);}
    for(unsigned i=0;i<4;i++){SDL_WaitThread(workers[i],&r);assert(!r);}
    assert(!diag_event_handle_acquire(h));assert(!diag_event_live_count());diag_event_report();
    SDL_Quit(); puts("SDL event lifetime, handle generation and concurrency checks passed (24000 cycles)");
}
