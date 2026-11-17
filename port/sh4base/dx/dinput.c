// Emulated DirectInput 8: keyboard (SDL) and gamepads (SDL GameController seen as one gamepad)
// DirectInput standard : stick gauche X/Y, stick droit Rx/Ry, croix = POV, 12 boutons).
#include "d3d8/d3d8.h"
#include "diag_bench_input.h"
#include <SDL.h>
#include <stdlib.h>
#ifdef __vita__
#include <psp2/ctrl.h>
#endif
/* SH4-specific pad policies, guest addresses and automation are omitted for SH3. */
typedef struct { ComObj com; } DInput;
int g_camera_pad_x, g_camera_pad_y, g_camera_pad_active;
unsigned g_camera_pad_frame;
typedef struct {
    ComObj com;
    int type;                  // 0 clavier, 1 souris, 2 manette
    int joy;                   // index SDL
    SDL_GameController *gc;
    uint32_t data_size;
    int32_t rmin[8], rmax[8];  // ranges per axis (offset/4: X Y Z Rx Ry Rz S0 S1)
    int32_t dead;              // zone morte (0..10000)
    unsigned neutral_generation;
    int await_neutral;
} DIDev;

#define M(p, name) static void p##_##name(CPU *c)
static const uint8_t GUID_KBD[16] = {0x61, 0x2b, 0x1d, 0x6f, 0xa0, 0xd5, 0xcf, 0x11, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0};
static const uint8_t GUID_MOUSE[16] = {0x60, 0x2b, 0x1d, 0x6f, 0xa0, 0xd5, 0xcf, 0x11, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0};
static const uint8_t GUID_JOY[12] = {'s', 'h', '3', 'v', 'i', 't', 'a', 'J', 'O', 'Y', 0, 0}; // +4 bytes: index

// ---------- clavier : scancode SDL -> DIK ----------
static uint8_t dik_of[SDL_NUM_SCANCODES];
static void init_dik(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *row1 = "QWERTYUIOP", *row2 = "ASDFGHJKL", *row3 = "ZXCVBNM";
    for (int i = 0; row1[i]; i++) dik_of[SDL_SCANCODE_A + row1[i] - 'A'] = (uint8_t)(0x10 + i);
    for (int i = 0; row2[i]; i++) dik_of[SDL_SCANCODE_A + row2[i] - 'A'] = (uint8_t)(0x1e + i);
    for (int i = 0; row3[i]; i++) dik_of[SDL_SCANCODE_A + row3[i] - 'A'] = (uint8_t)(0x2c + i);
    for (int i = 0; i < 9; i++) dik_of[SDL_SCANCODE_1 + i] = (uint8_t)(0x02 + i);
    dik_of[SDL_SCANCODE_0] = 0x0b;
    for (int i = 0; i < 10; i++) dik_of[SDL_SCANCODE_F1 + i] = (uint8_t)(0x3b + i);
    static const int pairs[][2] = {
        {SDL_SCANCODE_ESCAPE, 0x01}, {SDL_SCANCODE_MINUS, 0x0c}, {SDL_SCANCODE_EQUALS, 0x0d},
        {SDL_SCANCODE_BACKSPACE, 0x0e}, {SDL_SCANCODE_TAB, 0x0f}, {SDL_SCANCODE_LEFTBRACKET, 0x1a},
        {SDL_SCANCODE_RIGHTBRACKET, 0x1b}, {SDL_SCANCODE_RETURN, 0x1c}, {SDL_SCANCODE_LCTRL, 0x1d},
        {SDL_SCANCODE_SEMICOLON, 0x27}, {SDL_SCANCODE_APOSTROPHE, 0x28}, {SDL_SCANCODE_GRAVE, 0x29},
        {SDL_SCANCODE_LSHIFT, 0x2a}, {SDL_SCANCODE_BACKSLASH, 0x2b}, {SDL_SCANCODE_COMMA, 0x33},
        {SDL_SCANCODE_PERIOD, 0x34}, {SDL_SCANCODE_SLASH, 0x35}, {SDL_SCANCODE_RSHIFT, 0x36},
        {SDL_SCANCODE_KP_MULTIPLY, 0x37}, {SDL_SCANCODE_LALT, 0x38}, {SDL_SCANCODE_SPACE, 0x39},
        {SDL_SCANCODE_CAPSLOCK, 0x3a}, {SDL_SCANCODE_NUMLOCKCLEAR, 0x45}, {SDL_SCANCODE_SCROLLLOCK, 0x46},
        {SDL_SCANCODE_KP_7, 0x47}, {SDL_SCANCODE_KP_8, 0x48}, {SDL_SCANCODE_KP_9, 0x49},
        {SDL_SCANCODE_KP_MINUS, 0x4a}, {SDL_SCANCODE_KP_4, 0x4b}, {SDL_SCANCODE_KP_5, 0x4c},
        {SDL_SCANCODE_KP_6, 0x4d}, {SDL_SCANCODE_KP_PLUS, 0x4e}, {SDL_SCANCODE_KP_1, 0x4f},
        {SDL_SCANCODE_KP_2, 0x50}, {SDL_SCANCODE_KP_3, 0x51}, {SDL_SCANCODE_KP_0, 0x52},
        {SDL_SCANCODE_KP_PERIOD, 0x53}, {SDL_SCANCODE_F11, 0x57}, {SDL_SCANCODE_F12, 0x58},
        {SDL_SCANCODE_KP_ENTER, 0x9c}, {SDL_SCANCODE_RCTRL, 0x9d}, {SDL_SCANCODE_KP_DIVIDE, 0xb5},
        {SDL_SCANCODE_RALT, 0xb8}, {SDL_SCANCODE_HOME, 0xc7}, {SDL_SCANCODE_UP, 0xc8},
        {SDL_SCANCODE_PAGEUP, 0xc9}, {SDL_SCANCODE_LEFT, 0xcb}, {SDL_SCANCODE_RIGHT, 0xcd},
        {SDL_SCANCODE_END, 0xcf}, {SDL_SCANCODE_DOWN, 0xd0}, {SDL_SCANCODE_PAGEDOWN, 0xd1},
        {SDL_SCANCODE_INSERT, 0xd2}, {SDL_SCANCODE_DELETE, 0xd3}};
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) dik_of[pairs[i][0]] = (uint8_t)pairs[i][1];
}

// ---------- IDirectInputDevice8A ----------
M(dev, QueryInterface) { STDRET(3, 0x80004002u); }
M(dev, AddRef) { HOSTFN; ComObj *o = com_get(ARG(1)); STDRET(1, o ? ++o->refs : 0); }
M(dev, Release) {
    HOSTFN;
    DIDev *d = com_get_kind(ARG(1), K_DIDEV);
    if (!d) STDRET(1, 0);
    int32_t r = --d->com.refs;
    if (r <= 0) { if (d->gc) SDL_GameControllerClose(d->gc); com_free(&d->com); free(d); r = 0; }
    STDRET(1, (uint32_t)r);
}
M(dev, GetCapabilities) {
    HOSTFN; THIS(DIDev, K_DIDEV);
    uint32_t p = ARG(2);
    W32(p + 4, 1);                                  // DIDC_ATTACHED
    W32(p + 8, self->type == 0 ? 0x413 : self->type == 1 ? 0x12 : 0x215);
    W32(p + 12, self->type == 2 ? 6 : 0);           // axes
    W32(p + 16, self->type == 2 ? 12 : 0);          // boutons
    W32(p + 20, self->type == 2 ? 1 : 0);           // POV
    STDRET(2, HR_OK);
}
// The game (0x4065e0) sets each axis range to -1000..1000 here; without this call it reads 0..65535 and thinks the stick is pushed (centre 32768 > threshold 300).
M(dev, EnumObjects) {
    HOSTFN; THIS(DIDev, K_DIDEV);
    uint32_t cb = ARG(2), ref = ARG(3);
    if (self->type != 2) STDRET(4, HR_OK);
    uint32_t inst = gheap_calloc(316);   // DIDEVICEOBJECTINSTANCEA
    if (!inst) STDRET(4, 0x8007000eu); // E_OUTOFMEMORY, no callback with a null guest object
    for (uint32_t a = 0; a < 6; a++) {
        memset(mb + inst, 0, 316);
        W32(inst, 316);
        W32(inst + 20, a * 4);           // dwOfs in DIJOYSTATE
        W32(inst + 24, 2 | a << 8);      // DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(a)
        if (rt_guest_call(c, cb, 2, inst, ref) == 0) break;
    }
    gheap_free(inst);
    STDRET(4, HR_OK);
}
M(dev, GetProperty) { STDRET(3, HR_OK); }
M(dev, SetProperty) {
    HOSTFN; THIS(DIDev, K_DIDEV);
    uint32_t prop = ARG(2), ph = ARG(3), how = R32(ph + 12), obj = R32(ph + 8);
    if (prop == 4 || prop == 5) { // DIPROP_RANGE / DIPROP_DEADZONE
        for (int a = 0; a < 8; a++) {
            if (how == 1 && obj != (uint32_t)a * 4) continue; // DIPH_BYOFFSET
            if (how == 2 && (obj >> 8 & 0xffff) != (uint32_t)a) continue; // DIPH_BYID (dwType from EnumObjects)
            if (prop == 4) { self->rmin[a] = (int32_t)R32(ph + 16); self->rmax[a] = (int32_t)R32(ph + 20); }
            else self->dead = (int32_t)R32(ph + 16);
        }
    }
    STDRET(3, HR_OK);
}
M(dev, Acquire) { STDRET(1, HR_OK); }
M(dev, Unacquire) { STDRET(1, HR_OK); }

/* Diagnostic keyboard and basic SDL relative mouse; gamepad remains unimplemented. */
M(dev, GetDeviceState) {
    HOSTFN; THIS(DIDev, K_DIDEV);
    uint32_t n=ARG(2), p=ARG(3);
    if (!self || !p || p>g_mem_size || n>g_mem_size-p) rt_fatal("[dinput] invalid device state output");
    if (self->type==1) {
        if ((n!=16 && n!=20) || self->data_size!=n) rt_fatal("[dinput] unsupported mouse format size=%u data_size=%u",n,self->data_size);
        memset(mb+p,0,n);
#ifndef __vita__
        SDL_PumpEvents();
        int x=0,y=0; Uint32 buttons=SDL_GetRelativeMouseState(&x,&y);
        W32(p,(uint32_t)x); W32(p+4,(uint32_t)y);
        static const unsigned mouse_buttons[8]={1,3,2,4,5,6,7,8};
        for (unsigned i=0;i<(n==16?4u:8u);i++) if (buttons&SDL_BUTTON(mouse_buttons[i])) W8(p+12+i,0x80);
#endif
        /* Wheel events are not yet forwarded. */
        STDRET(3,HR_OK);
    }
    if (self->type!=0) rt_fatal("[dinput] gamepad state not yet adapted for SH3");
    memset(mb+p,0,n);
#ifdef __vita__
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositiveExt2(0,&pad,1)>0) {
        /* Ext2 keeps physical L1/R1 distinct from virtual L2/R2. Accept
           both shoulder representations without changing logical actions. */
        if(pad.buttons&SCE_CTRL_INTERCEPTED) {pad.buttons=0;pad.lx=pad.ly=pad.rx=pad.ry=128;}
        if (ENVFLAG("SH3_DIAG_PADLOG")) {
            static int up_seen, left_seen, triangle_seen, cross_previous, sticks_seen;
            static int right_stick_previous;
            if (!sticks_seen) {
                fprintf(stderr,"[diag] Vita initial sticks lx=%u ly=%u rx=%u ry=%u\n",
                        pad.lx,pad.ly,pad.rx,pad.ry);
                sticks_seen=1;
            }
            int right_stick=(pad.rx<64?1:0)|(pad.rx>192?2:0)|(pad.ry<64?4:0);
            if (right_stick!=right_stick_previous) {
                fprintf(stderr,"[diag] Vita right stick rx=%u ry=%u mask=%d at %llu ms\n",
                        pad.rx,pad.ry,right_stick,(unsigned long long)SDL_GetTicks64());
                right_stick_previous=right_stick;
            }
            if (!up_seen && (pad.buttons&SCE_CTRL_UP)) {
                fprintf(stderr,"[diag] Vita pad UP observed at %llu ms\n",(unsigned long long)SDL_GetTicks64());
                up_seen=1;
            }
            if (!triangle_seen && (pad.buttons&SCE_CTRL_TRIANGLE)) {
                fprintf(stderr,"[diag] Vita pad TRIANGLE observed at %llu ms\n",(unsigned long long)SDL_GetTicks64());
                triangle_seen=1;
            }
            if (!left_seen && (pad.buttons&SCE_CTRL_LEFT)) {
                fprintf(stderr,"[diag] Vita pad LEFT observed at %llu ms\n",(unsigned long long)SDL_GetTicks64());
                left_seen=1;
            }
            int cross_now=!!(pad.buttons&SCE_CTRL_CROSS);
            if (cross_now!=cross_previous) {
                fprintf(stderr,"[diag] Vita pad CROSS %s at %llu ms keyboard_bytes=%u\n",
                        cross_now?"pressed":"released",(unsigned long long)SDL_GetTicks64(),n);
                cross_previous=cross_now;
            }
        }
        /* The native main-state slot is 5 during gameplay. Pause stays in
           that state but sets bcc218; inventory/options have their own states.
           Do not mix camera arrows with movement keys as the old map did. */
        /* Cursor screens (puzzles...): while the left stick drives the virtual mouse (diag_runtime.c) it must not also press the
           move keys, and Cross is the mouse click, not Enter/Space (otherwise the confirmation would fire twice). */
        extern int diag_vcursor_stick_mode(void);
        const int cursor_mode=diag_vcursor_stick_mode();
        if(cursor_mode) pad.lx=pad.ly=128;
        uint32_t main_state=R32(0xbcc660u);
        int gameplay=main_state==5 && !R8(0xbcc218u);
        {   /* bounded pad trace (inventory investigation): every change of the button mask or of the menu/gameplay mapping */
            static unsigned prev_buttons=~0u,prev_mode=~0u,lines; unsigned mode=(unsigned)gameplay|(main_state<<1);
            if ((pad.buttons!=prev_buttons || mode!=prev_mode) && lines<300) { lines++;
                fprintf(stderr,"[pad] t=%llu buttons=%08x sticks=%u,%u,%u,%u main_state=%u gameplay=%d n=%u\n",(unsigned long long)SDL_GetTicks64(),(unsigned)pad.buttons,pad.lx,pad.ly,pad.rx,pad.ry,(unsigned)main_state,gameplay,(unsigned)n);
                prev_buttons=pad.buttons; prev_mode=mode; }
        }
#define VKEY(k) do { if(n>(k))W8(p+(k),0x80); } while(0)
        if(gameplay) {
            if(pad.ly<64)VKEY(0x11);      /* W: up (Type2D: the stick direction is the walking direction) */
            if(pad.ly>192)VKEY(0x1f);     /* S: down */
            if(pad.lx<64)VKEY(0x10);     /* Q: left (Type2D left/right are Q/E, checked on Vita 2026-11-16; A/D do nothing in 2D) */
            if(pad.lx>192)VKEY(0x12);    /* E: right */
            int look=pad.rx<64 || pad.rx>192 || pad.ry<64 || pad.ry>192;
            if(look)VKEY(0x0f);          /* native Search view, held while looking */
            if(pad.rx<64)VKEY(0xcb);     /* native view only */
            if(pad.rx>192)VKEY(0xcd);
            if(pad.ry<64)VKEY(0xc8);
            if(pad.ry>192)VKEY(0xd0);
            if(pad.buttons&SCE_CTRL_UP)VKEY(0x26);   /* flashlight */
            if(pad.buttons&SCE_CTRL_DOWN)VKEY(0x2c); /* native zoom, Z */
            if(pad.buttons&SCE_CTRL_LEFT)VKEY(0x10);   /* Type2D left (A/D side steps do nothing in 2D) */
            if(pad.buttons&SCE_CTRL_RIGHT)VKEY(0x12);  /* Type2D right */
        } else {
            /* Menus read the arrow keys AND the native Type3D keys W/S/Q/E (0x11/0x1f/0x10/0x12): the bench key
               injection (PAD_UP/DOWN/LEFT/RIGHT) sends both and navigates; arrows alone left the inventory
               cursor stuck on its tab bar (state 6, 2026-11-16). Menu states only: gameplay keeps its own map. */
            if((pad.buttons&SCE_CTRL_UP) || pad.ly<64){VKEY(0xc8);VKEY(0x11);}
            if((pad.buttons&SCE_CTRL_DOWN) || pad.ly>192){VKEY(0xd0);VKEY(0x1f);}
            if((pad.buttons&SCE_CTRL_LEFT) || pad.lx<64){VKEY(0xcb);VKEY(0x10);}
            if((pad.buttons&SCE_CTRL_RIGHT) || pad.lx>192){VKEY(0xcd);VKEY(0x12);}
        }
#undef VKEY
        if ((pad.buttons&SCE_CTRL_CROSS) && !cursor_mode) { if (n>0x1c) W8(p+0x1c,0x80); }
        if ((pad.buttons&SCE_CTRL_CROSS) && !cursor_mode) { if (n>0x39) W8(p+0x39,0x80); }
        /* Default PC bindings from the original CD readme; these remain
           experimental until tested with the game's configurable key.ini. */
        /* Start opens the pause menu; Circle is Cancel (Esc) in menus only - in gameplay it must not open the pause (Valentin 2026-11-16). */
        if ((pad.buttons&SCE_CTRL_START) || (!gameplay && (pad.buttons&SCE_CTRL_CIRCLE))) { if (n>1) W8(p+1,0x80); }
        if (gameplay && (pad.buttons&SCE_CTRL_CIRCLE)) { if (n>0x26) W8(p+0x26,0x80); } /* Circle = flashlight (L) in gameplay, Valentin 2026-11-16 */
        if (pad.buttons&SCE_CTRL_SQUARE) { if (n>0x2a) W8(p+0x2a,0x80); } /* left shift: run */
        if (pad.buttons&SCE_CTRL_TRIANGLE) { if (n>0x17) W8(p+0x17,0x80); } /* I: inventory */
        if (pad.buttons&SCE_CTRL_SELECT) { if (n>0x32) W8(p+0x32,0x80); } /* M: map */
        /* Native Options uses A/D for tabs. Reserve shoulders there so
           right stick remains camera-only and Gamma stays reachable. */
        int options_tabs=main_state==14 && R32(0xbcc664u)==3 && R32(0xbcc668u)==0;
        if(options_tabs) {
            if(pad.buttons&(SCE_CTRL_LTRIGGER|SCE_CTRL_L1)) {if(n>0x1e)W8(p+0x1e,0x80);}
            if(pad.buttons&(SCE_CTRL_RTRIGGER|SCE_CTRL_R1)) {if(n>0x20)W8(p+0x20,0x80);}
        } else {
            if (pad.buttons&(SCE_CTRL_LTRIGGER|SCE_CTRL_L1)) { if (n>0x0f) W8(p+0x0f,0x80); } /* tab: observe */
            if (pad.buttons&(SCE_CTRL_RTRIGGER|SCE_CTRL_R1)) { if (n>0x30) W8(p+0x30,0x80); } /* B: aim */
        }
    }
#else
    SDL_PumpEvents();
    int count=0; const Uint8 *keys=SDL_GetKeyboardState(&count); init_dik();
    for (int i=0; keys && i<count && i<SDL_NUM_SCANCODES; i++)
        if (keys[i] && dik_of[i] && dik_of[i]<n) W8(p+dik_of[i],0x80);
#endif
    /* Explicit test-only automation: a few Enter pulses, no game-specific memory writes. */
    if (ENVFLAG("SH3_DIAG_AUTOSTART") && n>0x1c && !diag_bench_input_active()) {
        static Uint64 start; static unsigned last=~0u;
        Uint64 now=SDL_GetTicks64(); if (!start) start=now;
        Uint64 elapsed=now-start;
        Uint64 move_at=120000;
        const char *move_setting=getenv("SH3_DIAG_MOVE_AT_MS");
        if (move_setting) {
            char *end=NULL; unsigned long value=strtoul(move_setting,&end,10);
            if (end && !*end && value>=2000 && value<=600000) move_at=(Uint64)value;
        }
        static int adaptive=-1,adaptive_done;
        if (adaptive<0) adaptive=getenv("SH3_DIAG_AUTOCONFIRM")!=NULL;
        if (adaptive && !adaptive_done && g_mem[0x070e66d9u]>=1 && g_mem[0x070e66d9u]<=39) {
            adaptive_done=1;
            fprintf(stderr,"[diag] adaptive confirmation stopped at stage=%u elapsed=%llu ms\n",g_mem[0x070e66d9u],(unsigned long long)elapsed);
        }
        unsigned period=adaptive?3000u:6000u;
        int pulse=elapsed>=14000 && elapsed<(adaptive?120000u:44000u) &&
                  (!adaptive || !adaptive_done) && ((elapsed-14000)%period)<(adaptive?400u:250u);
        if (pulse) {
            unsigned press=(unsigned)((elapsed-14000)/period);
            W8(p+0x1c,0x80);
            if (press!=last) { fprintf(stderr,"[diag] automated Enter pulse %u at %llu ms\n",press,(unsigned long long)elapsed); last=press; }
        }
        if (getenv("SH3_DIAG_LOAD_TEST") && getenv("SH3_DIAG_LOAD_MOVE") && move_at>=15000u &&
            elapsed>=move_at-15000u && elapsed<move_at-10000u && n>0x11) {
            static int moved;
            W8(p+0x11,0x80);
            if (!moved) { fprintf(stderr,"[diag] load-test pre-move W at %llu ms for 5 seconds\n",(unsigned long long)elapsed); moved=1; }
        }
        if (getenv("SH3_DIAG_LOAD_TEST") && elapsed>=move_at && elapsed<move_at+30500) {
            /* Mac diagnostic: pause, choose Load, then select the first slot. */
            static unsigned load_pulses;
            Uint64 delta=elapsed-move_at;
            unsigned index=(unsigned)(delta/2000u);
            static const unsigned keys[6]={0x01u,0xd0u,0xd0u,0x1cu,0x1cu,0x1cu};
            if (delta>=18000u && delta<18300u) index=4;
            if (delta>=30000u && delta<30300u) index=5;
            if (index<6 && (index<4 || (index==4 && delta>=18000u) || (index==5 && delta>=30000u)) && delta%2000u<300u && n>keys[index]) {
                W8(p+keys[index],0x80);
                if (!(load_pulses&(1u<<index))) {
                    fprintf(stderr,"[diag] load-test pulse %u DIK %02x at %llu ms\n",index,keys[index],(unsigned long long)elapsed);
                    load_pulses|=1u<<index;
                }
            }
        } else if (getenv("SH3_DIAG_SAVE_TEST") && elapsed>=move_at && elapsed<move_at+12500) {
            /* Mac diagnostic: open pause, choose Save, then inspect the save
               screen. All file writes remain within the diagnostic runtime. */
            static unsigned saved_pulses;
            Uint64 delta=elapsed-move_at;
            unsigned index=(unsigned)(delta/2000u);
            static const unsigned keys[4]={0x01u,0xd0u,0x1cu,0x1cu};
            if (getenv("SH3_DIAG_SAVE_WRITE") && delta>=12000u && delta<12300u) index=3;
            if (index<4 && (index<3 || (getenv("SH3_DIAG_SAVE_WRITE") && delta>=12000u)) && delta%2000u<300u && n>keys[index]) {
                W8(p+keys[index],0x80);
                if (index==3 && getenv("SH3_DIAG_SAVE_CROSS") && n>0x39) W8(p+0x39,0x80);
                if (!(saved_pulses&(1u<<index))) {
                    fprintf(stderr,"[diag] save-test pulse %u DIK %02x at %llu ms\n",index,keys[index],(unsigned long long)elapsed);
                    saved_pulses|=1u<<index;
                }
            }
        } else if (getenv("SH3_DIAG_AUTOMOVE") && !getenv("SH3_DIAG_SAVE_TEST") && !getenv("SH3_DIAG_LOAD_TEST") && elapsed>=move_at && elapsed<move_at+5000) {
            static int logged;
            const char *key=getenv("SH3_DIAG_MOVE_KEY");
            unsigned dik=key && !strcmp(key,"Q") ? 0x10u : key && !strcmp(key,"E") ? 0x12u :
                         key && !strcmp(key,"I") ? 0x17u : key && !strcmp(key,"ESC") ? 0x01u : 0x11u;
            if (n>dik) W8(p+dik,0x80);
            if (!logged) { fprintf(stderr,"[diag] automated DIK %02x held at %llu ms for 5 seconds\n",dik,(unsigned long long)elapsed); logged=1; }
        }
        if (getenv("SH3_DIAG_AUTOMOVE") || getenv("SH3_DIAG_MOVE_CONTROL")) {
            static int before,after;
            extern void diag_capture_request(unsigned which);
            if (elapsed>=move_at-2000 && !before) { diag_capture_request(1); before=1; }
            Uint64 after_delay=getenv("SH3_DIAG_LOAD_TEST")?45000u:getenv("SH3_DIAG_SAVE_WRITE")?16000u:7000u;
            if (elapsed>=move_at+after_delay && !after) { diag_capture_request(2); after=1; }
        }
    }
    diag_bench_input_apply(mb+p,n);
    STDRET(3,HR_OK);
}
M(dev, GetDeviceData) { HOSTFN; if (ARG(4)) W32(ARG(4), 0); STDRET(5, HR_OK); }
M(dev, SetDataFormat) { HOSTFN; THIS(DIDev, K_DIDEV); self->data_size = R32(ARG(2) + 12); STDRET(2, HR_OK); }
M(dev, SetEventNotification) { STDRET(2, HR_OK); }
M(dev, SetCooperativeLevel) { STDRET(3, HR_OK); }
M(dev, GetObjectInfo) { STDRET(4, 0x80070002u); }
M(dev, GetDeviceInfo) { STDRET(2, HR_OK); }
M(dev, RunControlPanel) { STDRET(3, HR_OK); }
M(dev, Initialize) { STDRET(4, HR_OK); }
M(dev, CreateEffect) { HOSTFN; W32(ARG(3), 0); STDRET(5, 0x80004001u); }
M(dev, EnumEffects) { STDRET(4, HR_OK); }
M(dev, GetEffectInfo) { STDRET(3, 0x80004001u); }
M(dev, GetForceFeedbackState) { HOSTFN; W32(ARG(2), 0); STDRET(2, HR_OK); }
M(dev, SendForceFeedbackCommand) { STDRET(2, HR_OK); }
M(dev, EnumCreatedEffectObjects) { STDRET(4, HR_OK); }
M(dev, Escape) { STDRET(2, 0x80004001u); }
M(dev, Poll) { STDRET(1, HR_OK); }
M(dev, SendDeviceData) { STDRET(5, HR_OK); }
M(dev, EnumEffectsInFile) { STDRET(5, HR_OK); }
M(dev, WriteEffectToFile) { STDRET(5, HR_OK); }
M(dev, BuildActionMap) { STDRET(4, 0x80004001u); }
M(dev, SetActionMap) { STDRET(4, 0x80004001u); }
M(dev, GetImageInfo) { STDRET(2, 0x80004001u); }
static const gfn dev_vt[] = {dev_QueryInterface, dev_AddRef, dev_Release, dev_GetCapabilities, dev_EnumObjects,
    dev_GetProperty, dev_SetProperty, dev_Acquire, dev_Unacquire, dev_GetDeviceState, dev_GetDeviceData,
    dev_SetDataFormat, dev_SetEventNotification, dev_SetCooperativeLevel, dev_GetObjectInfo, dev_GetDeviceInfo,
    dev_RunControlPanel, dev_Initialize, dev_CreateEffect, dev_EnumEffects, dev_GetEffectInfo,
    dev_GetForceFeedbackState, dev_SendForceFeedbackCommand, dev_EnumCreatedEffectObjects, dev_Escape, dev_Poll,
    dev_SendDeviceData, dev_EnumEffectsInFile, dev_WriteEffectToFile, dev_BuildActionMap, dev_SetActionMap,
    dev_GetImageInfo};

// ---------- IDirectInput8A ----------
M(di, QueryInterface) { STDRET(3, 0x80004002u); }
M(di, AddRef) { HOSTFN; ComObj *o = com_get(ARG(1)); STDRET(1, o ? ++o->refs : 0); }
M(di, Release) { HOSTFN; ComObj *o = com_get(ARG(1)); STDRET(1, o && o->refs > 1 ? --o->refs : 0); }
M(di, CreateDevice) {
    HOSTFN;
    const uint8_t *g = mb + ARG(2);
    W32(ARG(3), 0);
    DIDev *d = calloc(1, sizeof *d);
    if (!d) STDRET(4, 0x8007000eu);
    for (int a = 0; a < 8; a++) d->rmax[a] = 65535;
    if (!memcmp(g, GUID_KBD, 16)) d->type = 0;
    else if (!memcmp(g, GUID_MOUSE, 16)) d->type = 1;
    else if (!memcmp(g, GUID_JOY, 10)) {
        d->type = 2;
        d->joy = (int)R32(ARG(2) + 12);
        d->gc = SDL_GameControllerOpen(d->joy);
    } else {
        rt_log("[dinput] CreateDevice: unknown GUID %08x\n", R32(ARG(2)));
        free(d);
        W32(ARG(3), 0);
        STDRET(4, 0x80040154u);
    }
    rt_log("[dinput] CreateDevice type %d\n", d->type);
    W32(ARG(3), com_wrap(&d->com, K_DIDEV, dev_vt, 32, "IDirectInputDevice8A"));
    STDRET(4, HR_OK);
}
M(di, EnumDevices) {
    HOSTFN;
    uint32_t type = ARG(2), cb = ARG(3), ref = ARG(4);
    int want_pad = type == 0 || type == 4 || (type & 0xff) == 0x14 || (type & 0xff) == 0x15;
    if (!want_pad || ENVFLAG("SH3_DIAG_NOPAD")) STDRET(5, HR_OK);
    uint32_t inst = gheap_calloc(580);
    if (!inst) STDRET(5, 0x8007000eu);
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        memset(mb + inst, 0, 580);
        W32(inst, 580);
        memcpy(mb + inst + 4, GUID_JOY, 12);
        W32(inst + 16, (uint32_t)i);
        memcpy(mb + inst + 20, GUID_JOY, 12);
        W32(inst + 36, 0x215); // DI8DEVTYPE_GAMEPAD standard
        SDL_strlcpy(GSTR(inst + 40), SDL_GameControllerNameForIndex(i), 260);
        SDL_strlcpy(GSTR(inst + 300), SDL_GameControllerNameForIndex(i), 260);
        rt_log("[dinput] manette %d : %s\n", i, SDL_GameControllerNameForIndex(i));
        if (rt_guest_call(c, cb, 2, inst, ref) == 0) break; // DIENUM_STOP
    }
    gheap_free(inst);
    STDRET(5, HR_OK);
}
M(di, GetDeviceStatus) { STDRET(2, HR_OK); }
M(di, RunControlPanel) { STDRET(3, HR_OK); }
M(di, Initialize) { STDRET(3, HR_OK); }
M(di, FindDevice) { STDRET(4, 0x80040154u); }
M(di, EnumDevicesBySemantics) { STDRET(6, HR_OK); }
M(di, ConfigureDevices) { STDRET(5, 0x80004001u); }
static const gfn di_vt[] = {di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices,
    di_GetDeviceStatus, di_RunControlPanel, di_Initialize, di_FindDevice, di_EnumDevicesBySemantics,
    di_ConfigureDevices};

void imp_DINPUT8_DirectInput8Create(CPU *c) {
    HOSTFN;
    W32(ARG(4), 0);
    DInput *d = calloc(1, sizeof *d);
    if (!d) STDRET(5, 0x8007000eu);
    W32(ARG(4), com_wrap(&d->com, K_DINPUT, di_vt, 11, "IDirectInput8A"));
    rt_log("[dinput] DirectInput8Create (version %x)\n", ARG(2));
    STDRET(5, HR_OK);
}
