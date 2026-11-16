#include "diag_widescreen.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void rigid(float x,float y,float w,float h) {
    float q[]={x,y,x+w,y,x,y+h,x+w,y+h},before[8];memcpy(before,q,sizeof q);
    assert(diag_wide_inventory_layout(960,544,4,q,1));
    for(unsigned i=0;i<4;i++) {
        assert(fabsf((q[2*i]-before[2*i])-(q[0]-before[0]))<0.001f);
        assert(q[2*i+1]==before[2*i+1]);
    }
    assert(fabsf((q[2]-q[0])-w)<0.001f);
}
int main(void) {
    rigid(129,30,180,136);  /* live portrait */
    rigid(175,249,110,36); /* left category */
    rigid(530,94,107,126); /* item, including non-square aspect */
    rigid(600,270,205,80); /* action menu */
    /* Every glyph of each observed word must get the SAME translation;
       preserving individual glyph shape alone would still allow torn words. */
    const float rows[][4]={{365,5,470,25},{530,5,610,25},{670,5,790,25},{320,420,810,20}};
    for(unsigned r=0;r<4;r++) {
        float shift=NAN;
        for(float x=rows[r][0];x<rows[r][2];x+=14) {
            float q[]={x,rows[r][1],x+14,rows[r][1]+rows[r][3]};
            assert(diag_wide_inventory_layout(960,544,2,q,1));
            if(isnan(shift))shift=q[0]-x;
            assert(fabsf(q[0]-x-shift)<0.001f);rigid(x,rows[r][1],14,rows[r][3]);
        }
    }
    /* Multi-draw arrows must retain their internal offsets as a whole;
       recentering every small strip collapses the composite sprite. */
    const float centers[]={488,490,493,640,645,650,305,315,325};
    float last_shift=0;
    for(unsigned i=0;i<9;i++) {
        float q[]={centers[i]-2,i<6?35:270,centers[i]+2,i<6?37:272};
        assert(diag_wide_inventory_layout(960,544,2,q,0));
        float shift=q[0]-(centers[i]-2);
        if(i%3)assert(fabsf(shift-last_shift)<0.001f);
        last_shift=shift;
    }
    float bg[]={0,0,960,544},copy[4];memcpy(copy,bg,sizeof bg);
    assert(!diag_wide_inventory_layout(960,544,2,bg,0));assert(!memcmp(bg,copy,sizeof bg));
    assert(!diag_wide_inventory_layout(640,480,2,bg,0));assert(!memcmp(bg,copy,sizeof bg));
    bg[0]=NAN;assert(!diag_wide_inventory_layout(960,544,2,bg,0));
    puts("PASS: inventory groups/glyph words retain proportions and spacing; full-screen and 4:3 untouched");
}
