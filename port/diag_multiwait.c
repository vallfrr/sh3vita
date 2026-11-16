#include "diag_multiwait.h"
#include <SDL.h>
int diag_multiwait_events(DiagEvent *const *events,unsigned count,uint32_t timeout) {
    if(!events || !count || count>64)return -1;
    for(unsigned i=0;i<count;i++) if(!events[i])return -1;
    Uint64 start=SDL_GetTicks64();
    for(;;) {
        for(unsigned i=0;i<count;i++) {
            int r=diag_event_wait(events[i],0);
            if(r==0)return (int)i;
            if(r!=258)return -1;
        }
        if(timeout!=UINT32_MAX && SDL_GetTicks64()-start>=timeout)return 258;
        SDL_Delay(1);
    }
}
