/* SH3-only console wording. Guest files and unrelated messages unchanged. */
#include "x86rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uint32_t g_mem_size;
extern uint32_t gheap_alloc(uint32_t size);
extern void F_005fa400_orig(CPU *c);
static uint32_t quit_message;
static uint32_t console_text(const char *s) {   /* native message format: 00 80, ASCII-0x20, ff ff */
    size_t n=strlen(s);uint32_t q=gheap_alloc((uint32_t)n+4u);if(!q)return 0;
    g_mem[q]=0;g_mem[q+1]=0x80;
    for(size_t i=0;i<n;i++)g_mem[q+2+i]=(uint8_t)s[i]-0x20;
    g_mem[q+n+2]=g_mem[q+n+3]=0xff;
    return q;
}
/* Row 9 of Options (former "control type", ids 0x64 label, 0x65 help, 0x66/0x67 value) is the frame-rate setting:
   edited value 0xbcc1a8 = 0 (60), 1 (30), 2 (unlimited); see diag_fps_mode in diag_runtime.c. */
static uint32_t console_fps_message(uint32_t p,uint32_t id,unsigned language) {
    static const char *const label[5]={"Frame rate","Images par seconde","Bildrate","Fotogrammi","Im\xe1genes/s"};   /* Latin-1, as the game's own text */
    static const char *const help[5]={"Frame rate cap: 60, 30 or unlimited.","Limite d'images par seconde : 60, 30 ou sans limite.",
        "Bildrate: 60, 30 oder unbegrenzt.","Fotogrammi al secondo: 60, 30 o illimitati.","Im\xe1genes por segundo: 60, 30 o sin l\xedmite."};
    static const char *const unlimited[5]={"Unlimited","Sans limite","Unbegrenzt","Illimitati","Sin l\xedmite"};
    static uint32_t cached[5][5];   /* label, help, 60, 30, unlimited */
    unsigned kind=id==0x64u?0:id==0x65u?1:2+(g_mem[0xbcc1a8u]>2?0:g_mem[0xbcc1a8u]);
    if(!cached[language][kind]) {
        const char *s=kind==0?label[language]:kind==1?help[language]:kind==2?"60":kind==3?"30":unlimited[language];
        cached[language][kind]=console_text(s);
        if(!cached[language][kind])return p;
    }
    return cached[language][kind];
}
uint32_t diag_console_option_message(uint32_t p,uint32_t id,uint32_t caller) {
    if(p && caller>=0x5e3130u && caller<0x5e3344u && id>=0x64u && id<=0x67u) {
        /* Options: the language being edited (0xbcc1a1, applied live by F_00496780), not the saved one (0x70e66b1). */
        unsigned language=g_mem[0xbcc1a1u];language=language>=1 && language<=5?language-1:0;
        return console_fps_message(p,id,language);
    }
    if(!p || caller<0x5e3130u || caller>=0x5e3344u || (id!=0x2fu && id!=0x30u))return p;
    static const char *const text[5][2]={
        {"View controls","View the PS Vita controls."},
        {"Voir les commandes","Consultez les commandes de la PS Vita."},
        {"Tasten anzeigen","Tastenbelegung der PS Vita ansehen."},
        {"Vedi comandi","Consulta i comandi della PS Vita."},
        {"Ver controles","Consulta los controles de PS Vita."}
    };
    static uint32_t cached[5][2];
    unsigned language=g_mem[0xbcc1a1u];language=language>=1 && language<=5?language-1:0;
    unsigned kind=id==0x30u;
    if(!cached[language][kind]) {
        const char *s=text[language][kind];size_t n=strlen(s);
        uint32_t q=gheap_alloc((uint32_t)n+4u);if(!q)return p;
        g_mem[q]=0;g_mem[q+1]=0x80;
        for(size_t i=0;i<n;i++)g_mem[q+2+i]=(uint8_t)s[i]-0x20;
        g_mem[q+n+2]=g_mem[q+n+3]=0xff;
        cached[language][kind]=q;
    }
    return cached[language][kind];
}
uint32_t diag_console_pause_message(uint32_t p,uint32_t caller) {
    static int trace=-1; if (trace<0) trace=getenv("SH3_DIAG_CONSOLE_LABEL_TRACE")!=NULL;   /* called for every game text, every frame */
    if (trace) {
        static uint32_t seen[96],seen_p[96]; static unsigned count;
        unsigned i=0;
        for(;i<count;i++)if(seen[i]==caller && seen_p[i]==p)break;
        if(i==count && count<96 && p && (uint64_t)p+4<=g_mem_size) {
            seen[count]=caller; seen_p[count++]=p;
            char text[65]; unsigned n=0,off=g_mem[p]==0 && g_mem[p+1]==0x80 ? 2:0;
            while(n<64 && (uint64_t)p+off<g_mem_size) {
                unsigned ch=g_mem[p+off++]; if(ch==0xff || !ch)break;
                text[n++]=ch+0x20;
            }
            text[n]=0;
            fprintf(stderr,"[console-label] caller=%08x ptr=%08x text=%s\n",caller,p,text);
        }
    }
    if(caller!=0x5e8977u && caller!=0x5e8479u)return p;
    if(!p || (uint64_t)p+32>g_mem_size)return p;
    {   /* Pause entry 4 opens Options instead of the Inventory (Triangle still opens it): relabel it. */
        static const char *const inv[]={"Inventory","Inventaire","Inventar","Inventario"};
        static const char *const opt[]={"Options","Options","Optionen","Opzioni","Opciones"};
        static uint32_t cached[5];
        char t[16]; unsigned n=0,off=g_mem[p]==0 && g_mem[p+1]==0x80?2:0;
        while(n<15){unsigned ch=g_mem[p+off+n];if(ch==0xff||!ch)break;t[n++]=(char)(ch+0x20);}
        t[n]=0;
        for(unsigned i=0;i<4;i++) if(!strcmp(t,inv[i])) {
            unsigned lang=g_mem[0x70e66b1u];lang=lang>=1 && lang<=5?lang-1:0;
            if(!cached[lang]){const char *s=opt[lang];size_t k=strlen(s);uint32_t q=gheap_alloc((uint32_t)k+4u);if(!q)return p;
                g_mem[q]=0;g_mem[q+1]=0x80;for(size_t j=0;j<k;j++)g_mem[q+2+j]=(uint8_t)s[j]-0x20;g_mem[q+k+2]=g_mem[q+k+3]=0xff;cached[lang]=q;}
            return cached[lang];
        }
    }
    /* Native message format observed on this PE: 0x8000 prefix, Latin
       ASCII minus 0x20 bytes, 0xffff terminator (not a UTF-16 string). */
    static const uint8_t old[]={0x00,0x80,0x23,0x45,0x53,0x53,0x45,0x52,0xff,0xff};
    static const uint8_t replacement[]={0x00,0x80,0x31,0x55,0x49,0x54,0x54,0x45,0x52,0xff,0xff};
    if(memcmp(g_mem+p,old,sizeof old))return p;
    if(!quit_message){quit_message=gheap_alloc(sizeof replacement);if(!quit_message)return p;memcpy(g_mem+quit_message,replacement,sizeof replacement);fprintf(stderr,"[console-message] French pause Cesser -> Quitter\n");}
    return quit_message;
}
void F_005fa400(CPU *c) {
    uint32_t caller=0;
    if((uint64_t)c->esp+4<=g_mem_size)memcpy(&caller,g_mem+c->esp,4);
    F_005fa400_orig(c);
    c->eax=diag_console_pause_message(c->eax,caller);
}
