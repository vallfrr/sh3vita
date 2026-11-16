#include "diag_message.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
typedef struct { DiagMessageQueue *q; unsigned id; } Ctx;
static void *producer(void *arg) {
    Ctx *c=arg;
    for(unsigned i=0;i<1000;i++) {
        DiagMessage m={.hwnd=c->id,.message=i,.wparam=c->id^i}; int r;
        while((r=diag_message_post(c->q,&m))==1) sched_yield();
        assert(r==0);
    }
    return NULL;
}
int main(void) {
    assert(!diag_message_create(0));
    DiagMessageQueue *q=diag_message_create(2); assert(q);
    DiagMessage a={.message=1},b={.message=2},c={.message=3},m;
    assert(diag_message_peek(q,&m,1)==0);
    assert(!diag_message_post(q,&a)); assert(!diag_message_post(q,&b)); assert(diag_message_post(q,&c)==1);
    assert(diag_message_peek(q,&m,0)==1 && m.message==1);
    assert(diag_message_peek(q,&m,1)==1 && m.message==1);
    assert(!diag_message_post(q,&c));
    assert(diag_message_peek(q,&m,1)==1 && m.message==2);
    assert(diag_message_peek(q,&m,1)==1 && m.message==3);
    diag_message_destroy(q);
    q=diag_message_create(64);assert(q); pthread_t th[4]; Ctx ctx[4];
    for(unsigned i=0;i<4;i++){ctx[i]=(Ctx){q,i+1};assert(!pthread_create(&th[i],NULL,producer,&ctx[i]));}
    unsigned expected[5]={0};
    for(unsigned i=0;i<4000;) {
        int r=diag_message_peek(q,&m,1); assert(r>=0);
        if(!r){sched_yield();continue;}
        assert(m.hwnd>=1 && m.hwnd<=4 && m.message==expected[m.hwnd]++ && m.wparam==(m.hwnd^m.message));i++;
    }
    for(unsigned i=0;i<4;i++)assert(!pthread_join(th[i],NULL));
    assert(diag_message_peek(q,&m,1)==0);diag_message_destroy(q);
    puts("bounded message FIFO and producer ordering passed");
}
