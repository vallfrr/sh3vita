#include "diag_sync.h"
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
static int signaler(void *p) { SDL_Delay(30); return diag_sem_release(p,1,NULL); }
int main(void) {
    assert(!SDL_Init(SDL_INIT_TIMER));
    assert(!diag_sem_create(2,1));
    DiagSem *s=diag_sem_create(1,2); assert(s);
    assert(diag_sem_wait(s,0)==0); assert(diag_sem_wait(s,0)==258);
    SDL_Thread *t=SDL_CreateThread(signaler,"sem-test",s); assert(t);
    assert(diag_sem_wait(s,1000)==0); int status=-1; SDL_WaitThread(t,&status); assert(status==0);
    uint32_t previous=99; assert(!diag_sem_release(s,2,&previous) && previous==0);
    assert(diag_sem_release(s,1,NULL)==1);
    assert(!diag_sem_wait(s,0)); assert(!diag_sem_wait(s,0)); assert(diag_sem_wait(s,5)==258);
    diag_sem_destroy(s); SDL_Quit(); puts("SDL semaphore concurrency checks passed"); return 0;
}
