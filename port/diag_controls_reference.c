/* SH3 native fixed controller help. Artwork/backend reused from SH4;

 * no SH4 guest address or game asset is used. Native SH3 font and Cancel edge. */

#include "rt.h"

#include "d3d8/console_ui.h"

#include "d3d8/prep.h"

#ifdef SH3_CONTROLS_ARROWS_LAB
static const char *const words[5][15]={
 {"PS Vita controls","Look","Aim","Light / zoom","Move","Inventory","Pause / back","Run / guard","Action","Camera","Map","Back","Left / right: move","Light / back","Pause"},
 {"Commandes PS Vita","Observer","Viser","Lampe / zoom","Déplacer","Inventaire","Pause / retour","Courir","Action","Vue","Carte","Retour","Gauche / droite : déplacement","Lampe / retour","Pause"},
 {"PS Vita Steuerung","Umsehen","Zielen","Licht / Zoom","Bewegen","Inventar","Pause","Rennen","Aktion","Sicht","Karte","Zurueck","Links / rechts: bewegen","Licht / Zurueck","Pause"},
 {"Comandi PS Vita","Osserva","Mira","Luce / zoom","Muovi","Inventario","Pausa","Corri","Azione","Vista","Mappa","Indietro","Sinistra / destra: muovi","Luce / indietro","Pausa"},
 {"Controles PS Vita","Observar","Apuntar","Luz / zoom","Mover","Inventario","Pausa","Correr","Acción","Vista","Mapa","Volver","Izquierda / derecha: mover","Luz / volver","Pausa"}
};
#else
static const char *const words[5][15]={

 {"PS Vita controls","Look around","Aim","Up light / down zoom / L-R step","Left stick - move","Inventory","Pause / back","Run / guard","Action / examine","Right stick - view","Map","Back","Fixed controls"},

 {"Commandes PS Vita","Observer","Viser","Haut lampe / bas zoom / G-D pas","Stick gauche - déplacer","Inventaire","Pause / retour","Courir / garde","Action / examiner","Stick droit - vue","Carte","Retour","Commandes fixes"},

 {"PS Vita Steuerung","Umsehen","Zielen","Licht / Zoom / Schritt","Linker Stick - bewegen","Inventar","Pause / Zurueck","Rennen / Abwehr","Aktion / untersuchen","Rechter Stick - Sicht","Karte","Zurueck","Feste Steuerung"},

 {"Comandi PS Vita","Osserva","Mira","Luce / zoom / passi","Stick sinistro - muovi","Inventario","Pausa / indietro","Corri / guardia","Azione / esamina","Stick destro - vista","Mappa","Indietro","Comandi fissi"},

 {"Controles PS Vita","Observar","Apuntar","Luz / zoom / pasos","Stick izquierdo - mover","Inventario","Pausa / volver","Correr / defensa","Acción / examinar","Stick derecho - vista","Mapa","Volver","Controles fijos"}

};

#endif

static uint32_t strings[5][15];

static uint32_t label_string(unsigned language,unsigned key) {

    if(!strings[language][key]) {

        const unsigned char *s=(const unsigned char*)words[language][key];

        uint32_t n=(uint32_t)strlen((const char*)s)+1;

        uint32_t p=gheap_alloc(n),at=0;if(!p)rt_fatal("No memory for Vita control label");

        /* The native Western font parser consumes Latin-1 bytes, not UTF-8. */

        while(*s) {unsigned ch=*s++;

            if((ch==0xc2 || ch==0xc3) && *s>=0x80 && *s<=0xbf)ch=((ch&3)<<6)|(*s++&63);

            g_mem[p+at++]=(uint8_t)ch;

        }

        g_mem[p+at]=0;strings[language][key]=p;

    }

    return strings[language][key];

}

static void label(CPU*c,unsigned language,unsigned key,int x,int y) {

    /* Inverse of the centered native 512-wide interface mapping at 960x544.

       Keep glyph proportions; the Vita outline uses full framebuffer pixels. */

    int nx=256+(int)((x-480)*12.0f/17.0f);

    int ny=(int)(32+y*448.0f/544.0f);

    rt_guest_call(c,0x5fa540,2,key==0?18:12,key==0?30:26);

    rt_guest_call(c,0x5ee710,5,(uint32_t)nx,(uint32_t)ny,0,label_string(language,key),0);

    rt_guest_call(c,0x5fa570,0);

}

void diag_controls_reference_input(CPU*c) {

    HOSTFN;

    unsigned cancel=rt_guest_call(c,0x4dd4a0,1,1);

    W8(0xbcc359u,0);W8(0xbcc358u,0);

    if(cancel) {prep_wait_all();console_ui_release();}

    STDRET(0,cancel?R32(0x71f814u):0xffffffffu);

}

#ifdef SH3_CONTROLS_ARROWS_LAB
/* Same callout geometry as the SH4 Vita help page; labels remain SH3's. */
static void callout(float x1,float y1,float x2,float y2,float x3,float y3) {
    console_ui_line(x1,y1,x2,y2,0xffa5a5a5);
    console_ui_line(x2,y2,x3,y3,0xffa5a5a5);
}

void diag_controls_reference_draw(CPU*c) {
    unsigned language=g_mem[0xbcc1a1u];   /* language being edited in Options (applied live), as diag_console.c */
   language=language>=1&&language<=5?language-1:0;
    if(console_ui_begin()) {
        console_ui_fill(0,0,960,544,0xff000000);
        console_ui_sprite(UI_VITA,300,148,360,163.63f);
        callout(280,118,315,118,362,157);
        callout(680,118,645,118,598,157);
        callout(280,210,310,210,348,205);
        callout(280,350,310,350,357,246);
        callout(680,160,655,160,613,192);
        callout(680,200,655,200,598,205);
        callout(680,240,655,240,629,205);
        callout(680,280,655,280,613,219);
        callout(680,350,650,350,605,246);
        callout(599,274,570,390,490,390);
        callout(617,274,650,390,700,390);
        console_ui_line(40,463,920,463,0xff333333);
        console_ui_sprite(UI_CIRCLE,744,475,28,28);
        console_ui_end();
    }
    label(c,language,0,140,28);
    label(c,language,1,140,92);label(c,language,2,700,92);
    label(c,language,3,140,195);label(c,language,4,140,335);
    label(c,language,5,700,145);label(c,language,7,700,185);
    label(c,language,13,700,225);label(c,language,8,700,265);   /* Circle: flashlight in game, back in menus (dinput.c) */
    label(c,language,9,700,335);
    label(c,language,10,400,375);label(c,language,14,700,375);   /* Start: pause */
    label(c,language,12,140,483);label(c,language,11,780,483);
    STDRET(0,0);
}

#else
/* Retain the last readable SH3 layout until the arrow layout is qualified. */
void diag_controls_reference_draw(CPU*c) {
    unsigned language=g_mem[0xbcc1a1u];   /* language being edited in Options (applied live), as diag_console.c */
   language=language>=1&&language<=5?language-1:0;
    if(console_ui_begin()) {
        console_ui_fill(0,0,960,544,0xff000000);
        console_ui_sprite(UI_VITA,260,74,440,200);
        console_ui_sprite(UI_L,110,335,24,24);console_ui_sprite(UI_R,490,335,24,24);
        console_ui_sprite(UI_TRIANGLE,110,370,24,24);console_ui_sprite(UI_CROSS,490,370,24,24);
        console_ui_sprite(UI_SQUARE,110,405,24,24);console_ui_sprite(UI_CIRCLE,490,405,24,24);
        console_ui_sprite(UI_SELECT,110,440,24,24);console_ui_sprite(UI_START,490,440,24,24);
        console_ui_sprite(UI_CIRCLE,744,475,28,28);
        console_ui_sprite(UI_LEFT_STICK,110,295,24,24);console_ui_sprite(UI_RIGHT_STICK,490,295,24,24);
        console_ui_sprite(UI_DPAD,110,477,24,24);
        console_ui_line(40,474,920,474,0xff555555);
        console_ui_end();
    }
    label(c,language,0,140,28);
    label(c,language,4,140,300);label(c,language,9,520,300);
    label(c,language,1,140,335);label(c,language,2,520,335);
    label(c,language,5,140,370);label(c,language,8,520,370);
    label(c,language,7,140,405);label(c,language,6,520,405);
    label(c,language,10,140,440);label(c,language,6,520,440);
    label(c,language,3,140,483);label(c,language,11,780,483);
    STDRET(0,0);
}
#endif
