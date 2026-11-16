#include "diag_thread.h"
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
static uint32_t work(void *p) { SDL_Delay(30); return *(uint32_t *)p+7; }
typedef struct { DiagThread *thread; SDL_sem *ready; } SelfCtx;
static uint32_t self_work(void *p) {
    SelfCtx *c=p; SDL_SemWait(c->ready); assert(!diag_thread_self_suspend(c->thread)); return 73;
}
int main(void) {
    assert(!SDL_Init(SDL_INIT_TIMER));
    uint32_t arg=35,result=999;
    DiagThread *t=diag_thread_create(work,&arg); assert(t);
    assert(diag_thread_wait(t,0,&result)==258 && result==999);
    assert(diag_thread_join(t)==-1);
    assert(diag_thread_wait(t,1000,&result)==0 && result==42);
    assert(diag_thread_join(t)==0); assert(diag_thread_join(t)==-1);
    diag_thread_destroy(t);
    t=diag_thread_create_suspended(work,&arg); assert(t);
    assert(diag_thread_wait(t,20,NULL)==258); assert(diag_thread_resume(t)==1); assert(diag_thread_resume(t)==0);
    assert(!diag_thread_wait(t,1000,&result) && result==42); assert(!diag_thread_join(t)); diag_thread_destroy(t);
    SelfCtx ctx={0}; ctx.ready=SDL_CreateSemaphore(0); assert(ctx.ready);
    ctx.thread=diag_thread_create(self_work,&ctx); assert(ctx.thread); SDL_SemPost(ctx.ready);
    uint64_t deadline=SDL_GetTicks64()+1000;
    while(diag_thread_is_suspended(ctx.thread)!=1 && SDL_GetTicks64()<deadline) SDL_Delay(1);
    assert(diag_thread_is_suspended(ctx.thread)==1); assert(diag_thread_resume(ctx.thread)==1);
    assert(!diag_thread_wait(ctx.thread,1000,&result) && result==73); assert(!diag_thread_join(ctx.thread));
    diag_thread_destroy(ctx.thread); SDL_DestroySemaphore(ctx.ready);
    SDL_Quit(); puts("SDL thread lifecycle checks passed");
}
