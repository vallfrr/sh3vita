/* Diagnostic gate while the SH4 device/GL backend is being isolated. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t device_create(uint32_t presentation_parameters) {
    fprintf(stderr,"[diag] D3D8 CreateDevice parameters=%08x: GL backend not yet connected\n",presentation_parameters);
    exit(30);
}

/* The headless D3D probe has no real window; refuse rather than invent visibility. */
int diag_window_show(int show) { (void)show; fprintf(stderr,"[diag] ShowWindow unavailable in headless probe\n"); exit(30); }
void diag_window_ungrab(void) { fprintf(stderr,"[diag] ClipCursor unavailable in headless probe\n"); exit(30); }
int diag_window_focused(void) { return 0; }
int diag_display_mode(uint32_t index,uint32_t *w,uint32_t *h,uint32_t *bits,uint32_t *hz) {
    (void)index; (void)w; (void)h; (void)bits; (void)hz; return 0;
}
int diag_display_mode_supported(uint32_t w,uint32_t h,uint32_t bits,uint32_t hz) {
    (void)w; (void)h; (void)bits; (void)hz; return 0;
}
