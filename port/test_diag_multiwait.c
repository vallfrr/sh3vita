#include "diag_multiwait.h"
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
static int signaler(void *p) { SDL_Delay(20);return diag_event_set(p,1); }
int main(void) {
 assert(!SDL_Init(SDL_INIT_TIMER));
 DiagEvent *a=diag_event_create(0,0),*b=diag_event_create(0,0);assert(a && b);DiagEvent *e[]={a,b};
 assert(diag_multiwait_events(e,2,0)==258);assert(!diag_event_set(b,1));assert(diag_multiwait_events(e,2,0)==1);assert(diag_event_wait(b,0)==258);
 assert(!diag_event_set(a,1));assert(!diag_event_set(b,1));assert(diag_multiwait_events(e,2,0)==0);assert(diag_multiwait_events(e,2,0)==1);
 SDL_Thread *t=SDL_CreateThread(signaler,"signal",b);assert(t);assert(diag_multiwait_events(e,2,1000)==1);int r=-1;SDL_WaitThread(t,&r);assert(!r);
 diag_event_destroy(a);diag_event_destroy(b);SDL_Quit();puts("experimental event wait-any checks passed");
}
