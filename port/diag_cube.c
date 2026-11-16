/* Exploratory Direct3D8 cube allocation and sampling. GPL-3.0; incomplete. */
#include "d3d8.h"
#include "prep.h"
#include <stdlib.h>

typedef struct DiagCubeTag { ComObj com; GLuint texture; uint32_t face[6]; unsigned locked; } DiagCube;
/* Experimental lazy preparation is single-threaded here; the render queue
   must be disabled, and pending draws must finish before direct cube access. */
static int cube_lazy_enabled(void) {
    extern int g_prep_mode;
    const char *no_render_thread=getenv("SH4_NORTHREAD");
    return g_prep_mode==1 && getenv("SH3_DIAG_PREP_LAZY_SAFE") &&
           no_render_thread && !strcmp(no_render_thread,"1");
}
/* With a deferred consumer (lazy or thread) pending draws may still reference the cube. */
static void cube_flush_pending(void) { extern int g_prep_mode; if (g_prep_mode) prep_wait_all(); }
/* GL belongs to the D3D8 consumer: in sync mode prep_call runs the job at once on the game thread, with the
   preparation thread it runs there and the game thread waits for completion. */
typedef struct { struct DiagCubeTag *o; unsigned face; uint32_t edge; GLenum err; } CubeJob;
static void cube_job_delete(void *arg);
GLuint diag_cube_gl(ComObj *cube) {
    if (!cube || cube->kind!=K_CUBE || ((DiagCube *)cube)->locked) rt_fatal("[diag] invalid or locked cube binding");
    return ((DiagCube *)cube)->texture;
}
void diag_cube_release_obj(ComObj *cube) {
    DiagCube *o=(DiagCube *)cube;
    if (!o || o->com.kind!=K_CUBE || o->com.refs<=0) rt_fatal("[diag] invalid cube release");
    if (--o->com.refs) return;
    if (o->locked) rt_fatal("[diag] cannot release locked experimental cube");
    cube_flush_pending();
    CubeJob job={o,0,0,GL_NO_ERROR};
    prep_call(cube_job_delete,&job);
    for (unsigned i=0;i<6;i++) if (o->face[i]) gheap_free(o->face[i]);
    com_free(&o->com); free(o);
}
static void cube_job_delete(void *arg) { CubeJob *j=arg; glDeleteTextures(1,&j->o->texture); }
static void cube_job_upload(void *arg) {
    CubeJob *j=arg; DiagCube *o=j->o; unsigned face=j->face;
    GLenum prior=glGetError();
    if (prior!=GL_NO_ERROR) rt_log("[diag] pre-existing GL error before cube upload: %x\n",(unsigned)prior);
    GLint previous=0; glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP,&previous);
    GLenum query_err=glGetError();
    if (query_err!=GL_NO_ERROR) rt_fatal("[diag] cube binding query failed: %x",(unsigned)query_err);
    glBindTexture(GL_TEXTURE_CUBE_MAP,o->texture);
    GLenum bind_err=glGetError();
    if (bind_err!=GL_NO_ERROR) rt_fatal("[diag] cube bind failed: %x",(unsigned)bind_err);
#ifdef __vita__
    /* vitaGL's cube image path accepts RGBA, but rejects BGRA even though
       its ordinary 2D texture path accepts it. Preserve the D3D A8R8G8B8
       pixels by swapping red and blue in a temporary host buffer. */
    const unsigned char *src=g_mem+o->face[face];
    unsigned char *rgba=malloc(128u*128u*4u);
    if (!rgba) rt_fatal("[diag] cube conversion allocation failed");
    for (unsigned i=0;i<128u*128u;i++) {
        rgba[4*i]=src[4*i+2]; rgba[4*i+1]=src[4*i+1];
        rgba[4*i+2]=src[4*i]; rgba[4*i+3]=src[4*i+3];
    }
    glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,0,0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    free(rgba);
#else
    glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,0,0,0,128,128,GL_BGRA,GL_UNSIGNED_BYTE,g_mem+o->face[face]);
#endif
    j->err=glGetError(); glBindTexture(GL_TEXTURE_CUBE_MAP,(GLuint)previous);
}
static void cube_job_create(void *arg) {
    CubeJob *j=arg; DiagCube *o=j->o; uint32_t edge=j->edge;
    /* Drain any pre-existing GL errors so the allocation test is meaningful. */
    for (unsigned i=0; i<16 && glGetError()!=GL_NO_ERROR; i++) {}
    GLint previous=0; glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP,&previous);
    glGenTextures(1,&o->texture); glBindTexture(GL_TEXTURE_CUBE_MAP,o->texture);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    for (unsigned i=0;i<6;i++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+i,0,GL_RGBA,(GLsizei)edge,(GLsizei)edge,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    GLenum err=glGetError();
#ifndef __vita__
    for (unsigned i=0; i<6 && err==GL_NO_ERROR; i++) {
        GLint width=0,height=0;
        glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X+i,0,GL_TEXTURE_WIDTH,&width);
        glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X+i,0,GL_TEXTURE_HEIGHT,&height);
        if (width!=(GLint)edge || height!=(GLint)edge) err=GL_INVALID_OPERATION;
        else err=glGetError();
    }
#endif /* vitaGL does not expose glGetTexLevelParameteriv; verify on hardware. */
    glBindTexture(GL_TEXTURE_CUBE_MAP,(GLuint)previous);
    if (o->texture && err!=GL_NO_ERROR) { glDeleteTextures(1,&o->texture); o->texture=0; }
    j->err=err;
}
static void cube_missing(CPU *c) { (void)c; rt_fatal("[diag] cube method not implemented; no sampling/face access"); }
static void cube_query(CPU *c) { STDRET(3,0x80004002u); }
static void cube_addref(CPU *c) {
    HOSTFN; DiagCube *o=com_get_kind(ARG(1),K_CUBE);
    if (!o) rt_fatal("[diag] invalid cube AddRef");
    STDRET(1,++o->com.refs);
}
static void cube_release(CPU *c) {
    HOSTFN; DiagCube *o=com_get_kind(ARG(1),K_CUBE);
    if (!o || o->com.refs<=0) rt_fatal("[diag] invalid cube Release");
    int n=o->com.refs-1; diag_cube_release_obj(&o->com);
    STDRET(1,n);
}
static void cube_type(CPU *c) { STDRET(1,5); }
static void cube_lock(CPU *c) {
    HOSTFN; DiagCube *o=com_get_kind(ARG(1),K_CUBE); uint32_t face=ARG(2),level=ARG(3),out=ARG(4);
    if (!o || face>=6 || level || !out || out>g_mem_size-8 || ARG(5) || ARG(6) || (o->locked & (1u<<face)))
        rt_fatal("[diag] unsupported experimental cube lock");
    cube_flush_pending();
    if (!o->face[face]) o->face[face]=gheap_calloc(128u*128u*4u);
    if (!o->face[face]) rt_fatal("[diag] cube face allocation failed");
    W32(out,128u*4u); W32(out+4,o->face[face]); o->locked|=1u<<face;
    rt_log("[diag] experimental cube face %u locked\n",face); STDRET(6,HR_OK);
}
static void cube_unlock(CPU *c) {
    HOSTFN; DiagCube *o=com_get_kind(ARG(1),K_CUBE); uint32_t face=ARG(2),level=ARG(3);
    if (!o || face>=6 || level || !(o->locked & (1u<<face))) rt_fatal("[diag] unsupported experimental cube unlock");
    cube_flush_pending();
    CubeJob job={o,face,0,GL_NO_ERROR};
    prep_call(cube_job_upload,&job);
    GLenum err=job.err;
    if (err!=GL_NO_ERROR) rt_fatal("[diag] cube face upload failed: %x",(unsigned)err);
    o->locked&=~(1u<<face); rt_log("[diag] experimental cube face %u uploaded\n",face); STDRET(3,HR_OK);
}
static const gfn cube_vtable[19]={
    cube_query,cube_addref,cube_release,cube_missing,cube_missing,cube_missing,cube_missing,
    cube_missing,cube_missing,cube_missing,cube_type,cube_missing,cube_missing,cube_missing,
    cube_missing,cube_missing,cube_lock,cube_unlock,cube_missing
};
uint32_t diag_cube_create(uint32_t edge,uint32_t levels,uint32_t usage,uint32_t fmt,uint32_t pool) {
    extern int g_prep_mode;
    if (edge!=128 || levels!=1 || usage || fmt!=FMT_A8R8G8B8 || pool!=1 || (g_prep_mode==1 && !cube_lazy_enabled())) {
        rt_log("[diag] cube arguments or threading mode unsupported\n"); return 0;
    }
    cube_flush_pending();
    DiagCube *o=calloc(1,sizeof *o); if (!o) return 0;
    CubeJob job={o,0,edge,GL_NO_ERROR};
    prep_call(cube_job_create,&job);
    GLenum err=job.err;
    if (!o->texture || err!=GL_NO_ERROR) {
        rt_log("[diag] cube GL allocation failed: %x\n",(unsigned)err);
        free(o); return 0;
    }
    rt_log("[diag] experimental cube: six %ux%u GL faces allocated\n",edge,edge);
    return com_wrap(&o->com,K_CUBE,cube_vtable,19,"diagnostic cube");
}
