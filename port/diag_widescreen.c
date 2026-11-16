/* Geometry based on SilentHill3.FusionFix/Widescreen.ixx, WidescreenFixesPack,
 * ThirteenAG et al., GPL-3.0, commit fc975db74c99fef6b1a248fcd35816b48e65edf3.
 * Used by the opt-in native widescreen hooks and UI geometry experiments. */
#include "diag_widescreen.h"
#include <math.h>
unsigned g_diag_ui_background_fill=3;
unsigned g_diag_save_wide=1;
unsigned g_diag_inventory_wide=1;
unsigned g_diag_capture_uv_fix=1;

int diag_wide_box(DiagWideLayout layout, float w, float h, float *bw, float *bh) {
    if (!bw || !bh || !isfinite(w) || !isfinite(h) || w<=0 || h<=0 ||
        (layout!=DIAG_WIDE_INTERFACE && layout!=DIAG_WIDE_STRETCH && layout!=DIAG_WIDE_FILL)) return 0;
    int wider=w*3.0f>=h*4.0f;
    if (layout==DIAG_WIDE_STRETCH) { *bw=w; *bh=h; }
    else if (layout==DIAG_WIDE_FILL) { *bw=wider?w:h*(4.0f/3.0f); *bh=wider?w*0.75f:h; }
    else { *bw=wider?h*(4.0f/3.0f):w; *bh=wider?h:w*0.75f; }
    return 1;
}
int diag_wide_map(DiagWideLayout layout, float w, float h, int gx, int gy, float *x, float *y) {
    float bw,bh;
    if (!x || !y || !diag_wide_box(layout,w,h,&bw,&bh)) return 0;
    *x=w*0.5f+((float)gx/16.0f-2048.0f)*bw/512.0f;
    *y=h*0.5f+((float)gy/16.0f-2048.0f)*bh/448.0f;
    return 1;
}
float diag_wide_projection_x(float native, float w, float h) {
    if (!isfinite(native) || !isfinite(w) || !isfinite(h) || w<=0 || h<=0) return native;
    return native*(4.0f/3.0f)/(w/h);
}
int diag_wide_background_fill(unsigned screen,float w,float h,float xy[8]) {
    if((screen!=1 && screen!=14) || !xy || !isfinite(w) || !isfinite(h) || h<=0 || w/h<=1.34f)return 0;
    float xmin=INFINITY,ymin=INFINITY,xmax=-INFINITY,ymax=-INFINITY;
    for(unsigned i=0;i<4;i++){
        float x=xy[2*i],y=xy[2*i+1];if(!isfinite(x)||!isfinite(y))return 0;
        xmin=fminf(xmin,x);xmax=fmaxf(xmax,x);ymin=fminf(ymin,y);ymax=fmaxf(ymax,y);
    }
    float bw=h*(4.0f/3.0f),left=(w-bw)*0.5f,right=(w+bw)*0.5f;
    if(fabsf(xmin-left)>2 || fabsf(xmax-right)>2 || ymin>0.5f || ymax<h-0.5f)return 0;
    unsigned corners=0;
    for(unsigned i=0;i<4;i++){
        float x=xy[2*i],y=xy[2*i+1];int hi_x=fabsf(x-xmax)<0.01f,hi_y=fabsf(y-ymax)<0.01f;
        if((!hi_x && fabsf(x-xmin)>=0.01f)||(!hi_y && fabsf(y-ymin)>=0.01f))return 0;
        corners|=1u<<(hi_x+2*hi_y);
    }
    if(corners!=15)return 0;
    float scale=w/(xmax-xmin),cx=(xmin+xmax)*0.5f,cy=h*0.5f;
    for(unsigned i=0;i<4;i++){xy[2*i]=w*0.5f+(xy[2*i]-cx)*scale;xy[2*i+1]=cy+(xy[2*i+1]-cy)*scale;}
    return 1;
}

int diag_wide_inventory_layout(float w,float h,unsigned n,float *xy,int textured) {
    if(!xy || !n || n>32 || !isfinite(w) || !isfinite(h) || h<=0 || w/h<=1.34f)return 0;
    float xmin=INFINITY,ymin=INFINITY,xmax=-INFINITY,ymax=-INFINITY;
    for(unsigned i=0;i<n;i++) {
        float x=xy[2*i],y=xy[2*i+1];if(!isfinite(x)||!isfinite(y))return 0;
        xmin=fminf(xmin,x);xmax=fmaxf(xmax,x);ymin=fminf(ymin,y);ymax=fmaxf(ymax,y);
    }
    float scale=h/544.0f,extra=(w-h*(4.0f/3.0f))*0.5f;
    float cx=480.0f+((xmin+xmax)*0.5f-w*0.5f)/scale,cy=(ymin+ymax)*0.5f/scale;
    /* Whole-screen backgrounds, fades and unclassified broad panels stay put.
       Measured inventory sprites/text use individual small XYZRHW draws. */
    if(ymax-ymin>240*scale || (xmax-xmin>400*scale && cy>=70))return 0;
    float dx;
    if(cx<315 || (!textured && cx<350 && cy>=70 && xmax-xmin<40*scale && ymax-ymin<32*scale))dx=-extra*0.75f;                       /* status portrait, categories */
    else if(cy<70) {
        /* Small navigation sprites occupy the gaps between tab labels.
           Keep them in the expanded gaps rather than attaching them to the
           neighbouring word based on a single X threshold. */
        if(xmax-xmin<40*scale && ymax-ymin<32*scale && cx>480 && cx<520)dx=extra/12;
        else if(xmax-xmin<40*scale && ymax-ymin<32*scale && cx>630 && cx<670)dx=extra*(2.0f/3.0f);
        else if(xmax-xmin>200*scale) {        /* combined tab focus/background */
            float left=xmin-extra/3,right=xmax+extra;
            for(unsigned i=0;i<n;i++)xy[2*i]=left+(xy[2*i]-xmin)*(right-left)/(xmax-xmin);
            return 1;
        }
        else dx=cx<500?-extra/3:(cx<650?extra/3:extra);
    }
    else if(cy<370)dx=extra;                  /* item carousel and action menu */
    else dx=-extra*0.5f;                      /* descriptions, no font stretch */
    for(unsigned i=0;i<n;i++)xy[2*i]+=dx;
    return 1;
}

/* Save list cache: fill the full width while keeping font and icon quads
   unchanged. Native text remains centered in its original logical columns. */
int diag_wide_save_layout(float w,float h,unsigned n,float *xy,int glyph) {
    if(!xy || !n || n>64 || !isfinite(w) || !isfinite(h) || h<=0 || w/h<=1.34f)return 0;
    float xmin=INFINITY,xmax=-INFINITY,ymin=INFINITY,ymax=-INFINITY;
    for(unsigned i=0;i<n;i++) {
        if(!isfinite(xy[2*i]) || !isfinite(xy[2*i+1]))return 0;
        xmin=fminf(xmin,xy[2*i]);xmax=fmaxf(xmax,xy[2*i]);ymin=fminf(ymin,xy[2*i+1]);ymax=fmaxf(ymax,xy[2*i+1]);
    }
    float scale=h/544.0f;
    if(glyph && n==4 && xmax-xmin<40*scale && ymax-ymin<40*scale)return 0;
    float stretch=w/(h*(4.0f/3.0f));
    for(unsigned i=0;i<n;i++)xy[2*i]=w*.5f+(xy[2*i]-w*.5f)*stretch;
    return 1;
}
