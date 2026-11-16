#include "diag_widescreen.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static int eq(float a,float b) { return fabsf(a-b)<0.001f; }
int main(void) {
    float w,h,x,y;
    assert(diag_wide_box(DIAG_WIDE_INTERFACE,960,544,&w,&h));
    assert(eq(w,544.0f*4.0f/3.0f) && eq(h,544));
    assert(diag_wide_box(DIAG_WIDE_STRETCH,960,544,&w,&h) && eq(w,960) && eq(h,544));
    assert(diag_wide_box(DIAG_WIDE_FILL,960,544,&w,&h) && eq(w,960) && eq(h,720));
    assert(diag_wide_map(DIAG_WIDE_INTERFACE,960,544,2048*16,2048*16,&x,&y) && eq(x,480) && eq(y,272));
    assert(eq(diag_wide_projection_x(1,640,480),1));
    assert(eq(diag_wide_projection_x(1,960,540),0.75f));
    assert(!diag_wide_box(DIAG_WIDE_INTERFACE,0,544,&w,&h));
    puts("widescreen geometry checks passed");
}
