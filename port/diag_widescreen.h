#pragma once
#include <stdint.h>

/* SH3 aspect-ratio geometry used by native widescreen hooks. */
typedef enum { DIAG_WIDE_INTERFACE, DIAG_WIDE_STRETCH, DIAG_WIDE_FILL } DiagWideLayout;
int diag_wide_box(DiagWideLayout layout, float width, float height, float *box_w, float *box_h);
int diag_wide_map(DiagWideLayout layout, float width, float height, int gs_x, int gs_y, float *x, float *y);
float diag_wide_projection_x(float native, float width, float height);
/* Uniformly enlarge a title/options backdrop from the centered 4:3 region.
   Text, maps, gameplay and non-rectangular geometry must remain untouched. */
int diag_wide_background_fill(unsigned screen,float width,float height,float xy[8]);
extern unsigned g_diag_ui_background_fill;
/* Inventory-only layout experiment: translate groups, preserve local scale. */
int diag_wide_inventory_layout(float width,float height,unsigned count,float *xy,int textured);
extern unsigned g_diag_inventory_wide;

/* Diagnostic A/B control for the native pause snapshot UV compatibility fix. */
extern unsigned g_diag_capture_uv_fix;


extern unsigned g_diag_save_wide;
int diag_wide_save_layout(float w,float h,unsigned n,float *xy,int glyph);
