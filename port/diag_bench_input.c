/* Test-only keyboard input through the real DirectInput polling path. */
#include "diag_bench_input.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned enabled,reads;
static uint8_t keys[256];
void diag_bench_input_enable(int on) {
    pthread_mutex_lock(&lock);memset(keys,0,sizeof keys);
    __atomic_store_n(&enabled,!!on,__ATOMIC_RELEASE);pthread_mutex_unlock(&lock);
}
int diag_bench_input_active(void) {return __atomic_load_n(&enabled,__ATOMIC_ACQUIRE);}
unsigned diag_bench_input_reads(void) {pthread_mutex_lock(&lock);unsigned r=reads;pthread_mutex_unlock(&lock);return r;}
static int resolve_key(const char *name,unsigned *key_out,unsigned *extra_out) {
    static const struct {const char *name;unsigned key,extra;} map[]={
        {"ESC",1,0},{"ENTER",0x1c,0},{"SPACE",0x39,0},{"UP",0xc8,0},{"DOWN",0xd0,0},
        {"LEFT",0xcb,0},{"RIGHT",0xcd,0},{"TAB",0x0f,0},{"I",0x17,0},{"M",0x32,0},
        {"W",0x11,0},{"S",0x1f,0},{"A",0x1e,0},{"D",0x20,0},{"Q",0x10,0},{"E",0x12,0},
        {"L",0x26,0},{"B",0x30,0},{"SHIFT",0x2a,0},{"CTRL",0x1d,0},
        {"F1",0x3b,0},{"F2",0x3c,0},{"F5",0x3f,0},{"F9",0x43,0},
        {"PAD_UP",0xc8,0x11},{"PAD_DOWN",0xd0,0x1f},{"PAD_LEFT",0xcb,0x10},{"PAD_RIGHT",0xcd,0x12},
        {"CROSS",0x1c,0x39},{"CIRCLE",1,0},{"START",1,0},{"TRIANGLE",0x17,0},
        {"SELECT",0x32,0},{"SQUARE",0x2a,0},{"L1",0x0f,0},{"R1",0x30,0}};
    unsigned key=0,extra=0;
    for(unsigned i=0;i<sizeof map/sizeof map[0];i++)if(!strcasecmp(name,map[i].name)){key=map[i].key;extra=map[i].extra;break;}
    if(!key){char *end;unsigned long value=strtoul(name,&end,0);if(!*name || *end || !value || value>255)return -1;key=(unsigned)value;}
    *key_out=key;*extra_out=extra;return 0;
}
int diag_bench_input_key(const char *name,int down) {
    /* Validate the entire chord before changing anything; aliases may contain
       two scan codes. One mutex commit prevents a poll seeing half a chord. */
    unsigned chord[32],count=0;const char *part=name;
    if(!name || !*name)return -1;
    for(;;) {
        const char *end=strchr(part,'+');size_t n=end?(size_t)(end-part):strlen(part);
        char token[32];unsigned key,extra;
        if(!n || n>=sizeof token || count>30)return -1;
        memcpy(token,part,n);token[n]=0;
        if(resolve_key(token,&key,&extra))return -1;
        chord[count++]=key;if(extra)chord[count++]=extra;
        if(!end)break;part=end+1;
    }
    pthread_mutex_lock(&lock);
    for(unsigned i=0;i<count;i++)keys[chord[i]]=down?0x80:0;
    pthread_mutex_unlock(&lock);return 0;
}

void diag_bench_input_apply(uint8_t *state,unsigned bytes) {
    if(!diag_bench_input_active())return;
    pthread_mutex_lock(&lock);memset(state,0,bytes);memcpy(state,keys,bytes<256?bytes:256);reads++;pthread_mutex_unlock(&lock);
}
