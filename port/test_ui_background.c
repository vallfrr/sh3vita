#include "diag_widescreen.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    float before[8]={117.33333f,-38.857f,842.66667f,-38.857f,117.33333f,582.857f,842.66667f,582.857f},xy[8];
    memcpy(xy,before,sizeof xy);assert(diag_wide_background_fill(1,960,544,xy));
    assert(fabsf(xy[0])<.01f && fabsf(xy[2]-960)<.01f);
    float sx=(xy[2]-xy[0])/(before[2]-before[0]),sy=(xy[5]-xy[1])/(before[5]-before[1]);
    assert(fabsf(sx-sy)<.0001f);assert(xy[1]<0 && xy[5]>544);
    for(unsigned screen=0;screen<32;screen++)if(screen!=1 && screen!=14){memcpy(xy,before,sizeof xy);assert(!diag_wide_background_fill(screen,960,544,xy));assert(!memcmp(xy,before,sizeof xy));}
    memcpy(xy,before,sizeof xy);assert(diag_wide_background_fill(14,960,544,xy));
    memcpy(xy,before,sizeof xy);assert(!diag_wide_background_fill(1,640,480,xy));
    float glyph[8]={370,327,412,327,370,363,412,363};assert(!diag_wide_background_fill(1,960,544,glyph));
    memcpy(xy,before,sizeof xy);xy[0]=200;assert(!diag_wide_background_fill(1,960,544,xy));
    memcpy(xy,before,sizeof xy);xy[0]=NAN;assert(!diag_wide_background_fill(1,960,544,xy));
    puts("UI fill: uniform backdrop scale, original 4:3 and unrelated screens/glyphs preserved");
}
