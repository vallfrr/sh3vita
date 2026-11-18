/* Scripted in-process test bench ("bench"). Included by diag_runtime.c after rt_guest_call.
 * Script: Windows env SH3_BENCH=<file>, Vita ux0:data/sh3/diag/bench.txt. One command per line:
 *   peek HEX_ADDRESS BYTE_COUNT (read-only, 1..64 bytes)
 *   log TEXT | warp STAGE ROOM [X Z] | ready [MAXMS] | wait MS | shot NAME | shots NAME COUNT INTERVALMS | end
 * Every step is logged BEFORE it runs ("[bench] step N begin ...") and after ("[bench] step N done ..."), so a crash leaves the
 * last begun step in the journal. SH3_BENCH_START=<n> (Windows) / bench-start marker content (Vita) resumes at step n.
 * Warp = the game's own new-game path replayed in process: spawn descriptors, F_005d7620, F_005d7540, state stack top := 2. */
#define BENCH_MAX 4096
#include "diag_bench_input.h"
#include "diag_widescreen.h"
typedef struct { char cmd[16]; char arg[160]; } BenchStep;
static BenchStep bench_steps[BENCH_MAX]; static int bench_n=-1, bench_i, bench_started;
static uint64_t bench_t0, bench_step_t; static unsigned bench_frames, bench_step_frames, bench_series_done, bench_stable;
static unsigned bench_want_stage, bench_want_room; static int bench_warped, bench_started_movie, bench_resume;
extern void diag_bench_shot(const char *name);
extern void prep_wait_all(void); /* Drain queued presents/captures before normal bench exit. */
extern void com_resource_report(void);
static const char *bench_dir(void) {
#ifdef _WIN32
    const char *d=getenv("SH3_BENCH_OUT"); return d?d:"build/pc-win/bench";
#else
    return "ux0:data/sh3/diag/bench";
#endif
}
static unsigned bench_state_top(void) { uint32_t s=0; memcpy(&s,g_mem+0xbcc660u,4); return s; } /* main state: 1 title, 2 stage load, 3 init, 4/5 in game (array[1] at 0xbcc660) */
static volatile unsigned bench_progress_frames; static volatile int bench_cur_step=-1;
static const char *bench_progress_path(void) {
#ifdef _WIN32
    static char p[300]; snprintf(p,sizeof p,"%s/progress.txt",bench_dir()); return p;
#else
    return "ux0:data/sh3/diag/bench-progress";
#endif
}
static void bench_progress(const char *what,int i) { FILE *f=fopen(bench_progress_path(),"wb"); if (f) { fprintf(f,"%s %d\n",what,i); fclose(f); } }
static void *bench_watchdog(void *arg) {
    (void)arg; unsigned last=0; unsigned idle=0; unsigned limit=getenv("SH3_BENCH_HANG_S")?(unsigned)atoi(getenv("SH3_BENCH_HANG_S")):40u;
    for (;;) {
        SDL_Delay(1000); unsigned f=bench_progress_frames;
        if (f!=last) { last=f; idle=0; } else if (++idle>=limit && bench_cur_step>=0) {
            fprintf(stderr,"[bench] HANG: no game frame for %us during step %d; ending failed bench (no automatic restart)\n",idle,bench_cur_step);
            bench_progress("hang",bench_cur_step); fflush(stderr); _exit(87);
        }
    }
    return NULL;
}
static void bench_load(void) {
    bench_n=0; SH3_MKDIR(bench_dir(),0777);
#ifdef _WIN32
    const char *path=getenv("SH3_BENCH");
#else
    const char *path="ux0:data/sh3/diag/bench.txt";
#endif
    FILE *f=path?fopen(path,"rb"):NULL; if (!f) { bench_n=0; return; }
    char line[256];
    while (bench_n<BENCH_MAX && fgets(line,sizeof line,f)) {
        char *p=line; while (*p==' '||*p=='\t') p++;
        size_t L=strlen(p); while (L && (p[L-1]=='\n'||p[L-1]=='\r'||p[L-1]==' ')) p[--L]=0;
        if (!*p || *p=='#') continue;
        BenchStep *s=&bench_steps[bench_n]; char *sp=strchr(p,' ');
        size_t cl=sp?(size_t)(sp-p):strlen(p); if (cl>=sizeof s->cmd) cl=sizeof s->cmd-1;
        memcpy(s->cmd,p,cl); s->cmd[cl]=0; snprintf(s->arg,sizeof s->arg,"%s",sp?sp+1:""); bench_n++;
    }
    fclose(f);
#ifdef _WIN32
    const char *st=getenv("SH3_BENCH_START"); if (st) bench_i=atoi(st);
#else
    FILE *sf=fopen("ux0:data/sh3/diag/bench-start","rb"); if (sf) { int v=0; if (fscanf(sf,"%d",&v)==1) bench_i=v; fclose(sf); }
#endif
    { pthread_t th; if (!pthread_create(&th,NULL,bench_watchdog,NULL)) pthread_detach(th); }
    if (bench_i>3) { bench_resume=bench_i; bench_i=0; } /* resume: replay the 3 preamble steps (reach in-game), then jump */
    fprintf(stderr,"[bench] loaded %d steps from %s, start at %d, out dir %s\n",bench_n,path?path:"(none)",bench_i,bench_dir());
}
static void bench_warp(CPU *c,unsigned stage,unsigned room,int has_xz,float x,float z) {
    /* A diagnostic warp can leave an event while its script stream is open.
       The game's single-reader backend then rejects the next open, while the
       retained script code still treats its stale bytes as a valid header.
       Use the same cinematic teardown routines/order as F_0058ebb0 before
       replacing the scene. This only applies to the diagnostic warp path. */
    uint32_t script_active=0,stream_active=0,descriptor=0;
    memcpy(&script_active,g_mem+0xb6b88cu,4);
    memcpy(&stream_active,g_mem+0xbba06cu,4);
    memcpy(&descriptor,g_mem+0x898310u,4);
    if (script_active || stream_active || descriptor) {
        fprintf(stderr,"[bench] warp cinematic cleanup script=%u stream=%u descriptor=%x\n",script_active,stream_active,descriptor);
        rt_guest_call(c,0x4674e0u,0);
        rt_guest_call(c,0x499370u,0);
        memcpy(&stream_active,g_mem+0xbba06cu,4);
        if (stream_active) {
            fprintf(stderr,"[bench] warp cinematic cleanup FAILED: stream remains open\n");
            fflush(stderr);_exit(89);
        }
    }
    unsigned grid=g_mem[0x6b6f70+room]; int gx=grid>>4,gz=grid&15; if (gx>8) gx-=15; if (gz>8) gz-=15;
    if (!has_xz) { x=((float)gx-0.5f)*40000.0f; z=((float)gz-0.5f)*40000.0f; }
    for (unsigned i=0;i<2;i++) { uint8_t *d=g_mem+0x71e060+24*i; memcpy(d,&x,4); memcpy(d+8,&z,4); d[20]=(uint8_t)stage; }
    if (getenv("SH3_BENCH_MAPRESET")) rt_guest_call(c,0x49cb80u,0); if (getenv("SH3_BENCH_NEWGAME")) rt_guest_call(c,0x5d7620u,0); rt_guest_call(c,0x5d7540u,0); rt_guest_call(c,0x5f1b50u,0); rt_guest_call(c,0x5f1b70u,1,2u); rt_guest_call(c,0x5f1b60u,0);
}
static void bench_status(const char *tag,int i) {
    uint32_t ord=0; memcpy(&ord,g_mem+0x897e48u,4);
    fprintf(stderr,"[bench] step %d %s stage=%u room=%u state=%u frames=%u t=%llums\n",i,tag,g_mem[0x70e66d9],ord,bench_state_top(),bench_frames,(unsigned long long)(SDL_GetTicks64()-bench_t0));
#ifdef SH3_PARTICLE_NATIVE_LAB
    if (getenv("SH3_AUDIT_PARTICLE_DRAW_OFF") && i>=0 && i<bench_n &&
        !strcmp(bench_steps[i].cmd,"wait")) {
        extern unsigned g_audit_particle_draw_skipped;
        fprintf(stderr,"[audit-particle-window] step=%d %s omitted=%u\n",i,tag,
                __atomic_load_n(&g_audit_particle_draw_skipped,__ATOMIC_RELAXED));
    }
#endif
    fflush(stderr);
}
static unsigned bench_runtrace_left;   /* bench command runtrace N: per frame-limiter call log of the logic ticks and Heather's position */
static void bench_tick(CPU *c) {
    if (bench_n<0) { bench_load(); bench_t0=SDL_GetTicks64(); }
    if (!bench_n || bench_i>=bench_n) return;
    bench_frames++; bench_step_frames++; bench_progress_frames=bench_frames;
    if (bench_runtrace_left) {
        extern unsigned g_frame; uint32_t ticks; float dt,pos[3]; static uint64_t prev_us;
        memcpy(&ticks,g_mem+0x70e67a4u,4); memcpy(&dt,g_mem+0x70e67acu,4); memcpy(pos,g_mem+0x8984e0u,12);
        uint64_t us=SDL_GetPerformanceCounter()*1000000ull/SDL_GetPerformanceFrequency();
        uint32_t req_ticks,used_ticks,sim_ticks; memcpy(&req_ticks,g_mem+0x72c7e8u,4); memcpy(&used_ticks,g_mem+0x72c7f4u,4); memcpy(&sim_ticks,g_mem+0xbcc630u,4);
        fprintf(stderr,"[run] n=%u frame=%u t_us=%llu gap_us=%llu ticks=%u dt=%.6f pos=%.4f,%.4f,%.4f req=%u used=%u sim=%u\n",bench_runtrace_left,g_frame,(unsigned long long)us,(unsigned long long)(prev_us?us-prev_us:0),ticks,dt,pos[0],pos[1],pos[2],req_ticks,used_ticks,sim_ticks);
        prev_us=us; bench_runtrace_left--;
    }
    static uint32_t last_sig[6]; uint32_t sig[6]; unsigned st=bench_state_top(); memcpy(sig,g_mem+0xbcc658u,24); (void)st;
    if (memcmp(sig,last_sig,24)) { fprintf(stderr,"[bench] state stack depth=%u [%u %u %u %u %u] stage=%u at frame %u\n",sig[0],sig[1],sig[2],sig[3],sig[4],sig[5],g_mem[0x70e66d9],bench_frames); memcpy(last_sig,sig,24); }
    BenchStep *s=&bench_steps[bench_i]; uint64_t now=SDL_GetTicks64();
    if (!bench_started) { bench_started=1; bench_step_t=now; bench_step_frames=0; bench_series_done=0; bench_stable=0; bench_warped=0; bench_status("begin",bench_i); bench_cur_step=bench_i; bench_progress("begin",bench_i); fprintf(stderr,"[bench] step %d cmd=%s %s\n",bench_i,s->cmd,s->arg); }
    int done=0; uint64_t el=now-bench_step_t;
    if (!strcmp(s->cmd,"log")) { fprintf(stderr,"[bench] note: %s\n",s->arg); done=1; }
    else if (!strcmp(s->cmd,"end")) { prep_wait_all(); bench_status("end",bench_i); fflush(stderr); if (getenv("SH3_BENCH_NOEXIT")==NULL) { fprintf(stderr,"[bench] finished\n"); fflush(stderr); _exit(0); } done=1; }
    else if (!strcmp(s->cmd,"wait")) done=el>=(uint64_t)atoll(s->arg);
    else if (!strcmp(s->cmd,"fixturehp")) {
        /* Diagnostic damage seed only. Native F_004ec800 derives percent at
           +188 from current/max at player+180/+184; never patch the cache. */
        float expected=0,value=0,current=0,maximum=0,cached=0;unsigned ordinal=0;
        memcpy(&current,g_mem+0x898660u,4);memcpy(&maximum,g_mem+0x898664u,4);
        memcpy(&cached,g_mem+0x898668u,4);memcpy(&ordinal,g_mem+0x897e48u,4);
        if(sscanf(s->arg,"%f %f",&expected,&value)!=2 || !isfinite(expected) || !isfinite(value) ||
           !isfinite(current) || !isfinite(maximum) || !isfinite(cached) || maximum<=0 || maximum>1000000 ||
           current<0 || current>maximum || value<=0 || value>100 || fabsf(current*100.0f/maximum-expected)>0.001f ||
           fabsf(cached-expected)>0.001f || bench_state_top()!=5 || g_mem[0x70e66d9u]!=33 || ordinal!=210) {
            fprintf(stderr,"[bench] fixturehp rejected current=%g max=%g cached=%g expected=%g new=%g state=%u room=%u:%u\n",current,maximum,cached,expected,value,bench_state_top(),g_mem[0x70e66d9u],ordinal);fflush(stderr);_exit(89);
        }
        float raw=maximum*(value/100.0f);memcpy(g_mem+0x898660u,&raw,4);
        fprintf(stderr,"[bench] INJECTED HP fixture address=00898660 raw=%g->%g max=%g percent=%g->%g; not natural damage\n",current,raw,maximum,expected,value);done=1;
    }
    else if (!strcmp(s->cmd,"enemies") || !strcmp(s->cmd,"killenemies") ||
             !strcmp(s->cmd,"waitdead") || !strcmp(s->cmd,"approachcorpse")) {
        /* PC enemyWork: 32 entries, stride0x160, active+0x158,
           SubCharacter*+4 (F_004a3030/F_004a30d0). Common HP+0x180. */
        unsigned ordinal=0,found=0,killed=0,corpses=0;float player[3];memcpy(player,g_mem+0x8984e0u,12);memcpy(&ordinal,g_mem+0x897e48u,4);
        if(bench_state_top()!=5 || g_mem[0x70e66d9u]!=2 || ordinal!=11) {
            fprintf(stderr,"[bench] enemy fixture requires gameplay2:11\n");fflush(stderr);_exit(89);
        }
        for(unsigned i=0;i<32;i++) {
            uint32_t work=0x71294c0u+0x160u*i,p=0,ai=0;uint16_t kind=0,mode=0;float hp=0,maximum=0,pos[3];
            if(!g_mem[work+0x158])continue;
            memcpy(&p,g_mem+work+4,4);memcpy(&ai,g_mem+work,4);
            if(p==0x8984e0u || p>g_mem_size-0x1b0u)continue;
            memcpy(&kind,g_mem+p+0x80,2);memcpy(&hp,g_mem+p+0x180,4);memcpy(&maximum,g_mem+p+0x184,4);
            memcpy(&mode,g_mem+work+0x152,2);memcpy(pos,g_mem+p,12);
            if(kind<0x200 || kind>=0x300 || !isfinite(hp) || !isfinite(maximum) || maximum<=0 || maximum>1000000 || hp>maximum)continue;
            float dx=pos[0]-player[0],dz=pos[2]-player[2];
            if(!isfinite(dx) || !isfinite(dz) || dx*dx+dz*dz>2000.0f*2000.0f)continue;
            found++;
            unsigned corpse=(kind==0x201 && hp<=0 && (mode==31 || mode==32));
            corpses+=corpse;
            if(!strcmp(s->cmd,"approachcorpse") && corpse && corpses==1) {
                /* Deterministic placement beside the real corpse. Character
                   position and previous position are common native fields. */
                float place[3]={pos[0]-100.0f,pos[1],pos[2]},yaw=1.57079632679f;
                memcpy(g_mem+0x8984e0u,place,12);memcpy(g_mem+0x8984e0u+0xd0u,place,12);
                memcpy(g_mem+0x8984e0u+0x14u,&yaw,4);
                fprintf(stderr,"[bench] INJECTED player beside corpse actor=%08x pos=%g,%g,%g yaw=%g\n",p,place[0],place[1],place[2],yaw);
            }
            unsigned injected=0;
            if(!strcmp(s->cmd,"killenemies") && hp>0 && kind==0x201 && ai==0x557eb0u) {
                float zero=0;memcpy(g_mem+p+0x180,&zero,4);
                /* Numb Body native knockdown21 completes into dead state32
                   when HP<=0 (F_00557770, 00557883..005578a6). HP alone
                   does not interrupt its current AI state. */
                CPU saved=*c;rt_guest_call(c,0x5d2550u,2,work,21u);
                rt_guest_call(c,0x5d2530u,2,work,0u);*c=saved;killed++;injected=1;
            }
            if(strcmp(s->cmd,"waitdead") || bench_step_frames==1 || corpse)fprintf(stderr,"[bench-enemy] slot=%u work=%08x actor=%08x kind=%x ai=%08x mode=%u hp=%g/%g pos=%g,%g,%g injected_dead=%u\n",
                    i,work,p,kind,ai,mode,hp,maximum,pos[0],pos[1],pos[2],injected);
        }
        if(strcmp(s->cmd,"waitdead"))fprintf(stderr,"[bench-enemy] found=%u killed=%u\n",found,killed);
        if(!strcmp(s->cmd,"killenemies") && !found){fprintf(stderr,"[bench] no enemy to kill\n");fflush(stderr);_exit(89);}
        if(!strcmp(s->cmd,"approachcorpse") && !corpses){fprintf(stderr,"[bench] no native corpse to approach\n");fflush(stderr);_exit(89);}
        if(!strcmp(s->cmd,"waitdead")) {
            if(corpses)bench_stable++;else bench_stable=0;
            done=bench_stable>=30;
            unsigned limit=(unsigned)atoi(s->arg);if(!limit)limit=60000;
            if(!done && el>limit){fprintf(stderr,"[bench] native death TIMEOUT\n");fflush(stderr);_exit(89);}
        } else done=1;
    }
    else if (!strcmp(s->cmd,"fixturefoot") || !strcmp(s->cmd,"fixturedecal")) {
        /* Explicit diagnostic fixture: create native type11 at a fixed world
           position, using the same descriptor as F_005c7fd0. No save writes. */
        float dx=0,dz=0,pos[4];uint32_t room=0;unsigned type=11;
        memcpy(&room,g_mem+0x897e48u,4);memcpy(pos,g_mem+0x8984e0u,12);
        if(sscanf(s->arg,"%f %f %u",&dx,&dz,&type)<2 || (type!=7 && type!=9 && type!=11) || !isfinite(dx) || !isfinite(dz) ||
           fabsf(dx)>1000 || fabsf(dz)>1000 || bench_state_top()!=5 ||
           g_mem[0x70e66d9u]!=2 || room!=11 || c->esp<512 || c->esp>g_mem_size) {
            fprintf(stderr,"[bench] fixturefoot rejected\n");fflush(stderr);_exit(89);
        }
        pos[0]+=dx;pos[2]+=dz;pos[3]=1;
        uint32_t descriptor=c->esp-128,words[16]={0};uint8_t previous[64];
        words[1]=type;memcpy(words+4,pos,16);words[13]=0x3f800000u;
        /* +0x3c=0 keeps the diagnostic mark from fading during the orbit. */
        memcpy(previous,g_mem+descriptor,64);memcpy(g_mem+descriptor,words,64);
        CPU saved=*c;c->esp=descriptor-32;
        uint32_t result=rt_guest_call(c,0x4e9960u,1,descriptor);
        *c=saved;memcpy(g_mem+descriptor,previous,64);
        fprintf(stderr,"[bench] INJECTED native decal type=%u lifetime_flag=0 pos=%g,%g,%g result=%x\n",type,pos[0],pos[1],pos[2],result);
        done=1;
    }
    else if (!strcmp(s->cmd,"peek")) {
        unsigned address=0,count=0;
        if(sscanf(s->arg,"%x %u",&address,&count)!=2 || !count || count>64 || (uint64_t)address+count>g_mem_size) {
            fprintf(stderr,"[bench] invalid peek range\n");fflush(stderr);_exit(89);
        }
        char hex[129];static const char digits[]="0123456789abcdef";
        for(unsigned i=0;i<count;i++){unsigned v=g_mem[address+i];hex[2*i]=digits[v>>4];hex[2*i+1]=digits[v&15];}
        hex[2*count]=0;
        fprintf(stderr,"[bench] memory address=%08x bytes=%u hex=%s\n",address,count,hex);done=1;
    }
    else if (!strcmp(s->cmd,"input")) {diag_bench_input_enable(strcmp(s->arg,"off")!=0);done=1;}
    else if (!strcmp(s->cmd,"drawlog")) {extern void diag_drawlog_next(void);diag_drawlog_next();done=1;}
    else if (!strcmp(s->cmd,"runtrace")) {unsigned n=(unsigned)atoi(s->arg); bench_runtrace_left=n>0 && n<=6000?n:600; done=1;}
    else if (!strcmp(s->cmd,"uisave")) {__atomic_store_n(&g_diag_save_wide,strcmp(s->arg,"off")!=0,__ATOMIC_RELAXED);done=1;}
    else if (!strcmp(s->cmd,"uifill")) {__atomic_store_n(&g_diag_ui_background_fill,!strcmp(s->arg,"stretch")?3u:!strcmp(s->arg,"footer")?2u:(strcmp(s->arg,"off")!=0),__ATOMIC_RELAXED);done=1;}
    else if (!strcmp(s->cmd,"uiinventory")) {__atomic_store_n(&g_diag_inventory_wide,strcmp(s->arg,"off")!=0,__ATOMIC_RELAXED);done=1;}
    else if (!strcmp(s->cmd,"trace")) {extern void diag_trace_next_frames(void);diag_trace_next_frames();done=1;}
    else if (!strcmp(s->cmd,"keydown") || !strcmp(s->cmd,"keyup")) {
        if(diag_bench_input_key(s->arg,!strcmp(s->cmd,"keydown"))) {
            fprintf(stderr,"[bench] unknown held key %s\n",s->arg);fflush(stderr);_exit(89);
        }
        done=1;
    }
    else if (!strcmp(s->cmd,"key")) {
        char name[32];unsigned duration=200;static unsigned begin_reads;
        static uint64_t held_start,held_frames,held_ticks;static double held_seconds;
        static float held_position[3];static unsigned held_stage,held_room;static int held_game;
        if(sscanf(s->arg,"%31s %u",name,&duration)<1 || duration>10000 || !diag_bench_input_active()) {
            fprintf(stderr,"[bench] invalid key command or input disabled\n");fflush(stderr);_exit(89);
        }
        if(!bench_warped){if(diag_bench_input_key(name,1)){fprintf(stderr,"[bench] unknown key %s\n",name);fflush(stderr);_exit(89);}begin_reads=diag_bench_input_reads();bench_warped=1;
            /* Read-only motion ledger. Step logging/progress writes precede the
               actual key press, so their timestamps do not measure held time. */
            held_start=SDL_GetTicks64();held_frames=held_ticks=0;held_seconds=0;
            memcpy(held_position,g_mem+0x8984e0u,sizeof held_position);
            held_stage=g_mem[0x70e66d9u];memcpy(&held_room,g_mem+0x897e48u,4);held_game=bench_state_top()==5;
        } else {
            uint32_t ticks,room;float dt;memcpy(&ticks,g_mem+0x70e67a4u,4);memcpy(&dt,g_mem+0x70e67acu,4);
            memcpy(&room,g_mem+0x897e48u,4);held_frames++;held_ticks+=ticks;held_seconds+=dt;
            if(bench_state_top()!=5 || g_mem[0x70e66d9u]!=held_stage || room!=held_room)held_game=0;
        }
        if(el>=duration && diag_bench_input_reads()-begin_reads>=2){diag_bench_input_key(name,0);done=1;
            uint64_t held_end=SDL_GetTicks64();float position[3];memcpy(position,g_mem+0x8984e0u,sizeof position);
            fprintf(stderr,"[bench-motion] key=%s held_ms=%llu frames=%llu ticks=%llu sim_seconds=%.9f gameplay=%d stage=%u room=%u from=%.6f,%.6f,%.6f to=%.6f,%.6f,%.6f\n",
                name,(unsigned long long)(held_end-held_start),(unsigned long long)held_frames,(unsigned long long)held_ticks,held_seconds,held_game,held_stage,held_room,
                held_position[0],held_position[1],held_position[2],position[0],position[1],position[2]);
        }
        else if(el>10000){fprintf(stderr,"[bench] keyboard not polled for command %s\n",name);fflush(stderr);_exit(89);}
    }
    else if (!strcmp(s->cmd,"captureuv")) { __atomic_store_n(&g_diag_capture_uv_fix,!strcmp(s->arg,"on"),__ATOMIC_RELAXED);done=1; }
    else if (!strcmp(s->cmd,"tracecopy")) { unsigned ms=(unsigned)atoi(s->arg); if(!ms || ms>30000)ms=10000;diag_trace_pause_for(ms);done=1; }
    else if (!strcmp(s->cmd,"poll")) {
        static uint64_t last_poll,last_control_hash;static unsigned batch;
        if(now-last_poll<250)return;last_poll=now;
        char path[320];
#ifdef _WIN32
        snprintf(path,sizeof path,"%s/next.txt",bench_dir());
#else
        snprintf(path,sizeof path,"ux0:data/sh3/diag/bench-next.txt");
#endif
        FILE *f=fopen(path,"rb");if(!f)return;
        BenchStep next[256];unsigned n=0;char line[256];int valid=1;uint64_t hash=14695981039346656037ull;
        while(fgets(line,sizeof line,f)){
            for(const unsigned char *q=(const unsigned char *)line;*q;q++){hash^=*q;hash*=1099511628211ull;}
            if(!strchr(line,'\n') && !feof(f)){valid=0;break;}
            char *p=line;while(*p==' '||*p=='\t')p++;size_t len=strlen(p);while(len && (p[len-1]=='\n'||p[len-1]=='\r'||p[len-1]==' '))p[--len]=0;
            if(!*p || *p=='#')continue;
            char *space=strchr(p,' ');size_t cmd=space?(size_t)(space-p):len;
            if(n>=255 || cmd>=sizeof next[0].cmd){valid=0;break;}
            memcpy(next[n].cmd,p,cmd);next[n].cmd[cmd]=0;snprintf(next[n].arg,sizeof next[n].arg,"%s",space?space+1:"");n++;
        }
        if(ferror(f))valid=0;fclose(f);
        if(valid && hash==last_control_hash)return;
        if(!valid || !n || bench_i+(int)n+1>=BENCH_MAX){
            fprintf(stderr,"[bench] control script rejected\n");fflush(stderr);_exit(89);
        }
        /* Keep the published file intact: native upload/move transactions
           verify the target after committing. Deleting it here races their
           verification and strands the durable operation in recovery state.
           A unique comment in each host batch permits identical command sets
           without re-executing an already accepted publication. */
        last_control_hash=hash;
        memcpy(bench_steps+bench_i,next,n*sizeof next[0]);
        memset(bench_steps+bench_i+n,0,sizeof next[0]);strcpy(bench_steps[bench_i+n].cmd,"poll");
        bench_n=bench_i+(int)n+1;bench_started=0;
        fprintf(stderr,"[bench] control batch %u accepted %u commands\n",++batch,n);fflush(stderr);return;
    }
    else if (!strcmp(s->cmd,"warp")) {
        unsigned a=0,b=0; float x=0,z=0; int n=sscanf(s->arg,"%u %u %f %f",&a,&b,&x,&z);
        if (n<2 || a<1 || a>39 || b<1 || b>266) { fprintf(stderr,"[bench] step %d bad warp args\n",bench_i); done=1; }
        else { bench_warp(c,a,b,n>=4,x,z); bench_want_stage=a; bench_want_room=b; done=1; }
    }
    else if (!strcmp(s->cmd,"move")) {
        unsigned a=0,b=0; if (sscanf(s->arg,"%u %u",&a,&b)<2 || a!=g_mem[0x70e66d9]) { fprintf(stderr,"[bench] step %d move needs the current stage (%u)",bench_i,(unsigned)g_mem[0x70e66d9]); putc(10,stderr); done=1; }
        else {
            unsigned grid=g_mem[0x6b6f70+b]; int gx=grid>>4,gz=grid&15; if (gx>8) gx-=15; if (gz>8) gz-=15;
            float x=((float)gx-0.5f)*40000.0f,z=((float)gz-0.5f)*40000.0f; uint8_t *d=g_mem+0x71e060; memcpy(d,&x,4); memcpy(d+8,&z,4);
            uint32_t y=0; memcpy(&y,d+0x10,4); rt_guest_call(c,0x495b00u,2,0x71e060u,y); rt_guest_call(c,0x465370u,0);
            bench_want_stage=a; bench_want_room=b; done=1;
        }
    }
    else if (!strcmp(s->cmd,"ready")) {
        uint64_t max=s->arg[0]?(uint64_t)atoll(s->arg):60000; uint32_t ord=0; memcpy(&ord,g_mem+0x897e48u,4);
        static unsigned prev_state; unsigned cur=bench_state_top();
        if (g_mem[0x70e66d9]==bench_want_stage && cur==5 && cur==prev_state && (!getenv("SH3_BENCH_ORD") || ord==bench_want_room)) bench_stable++; else bench_stable=0;
        prev_state=cur;
        if (bench_stable>=45 && el>=3000) { { uint32_t cnt=0; memcpy(&cnt,g_mem+0xb71becu,4); fprintf(stderr,"[bench] pools(count=%u):",cnt); for (unsigned k=0;k<39;k++) { uint32_t e[7]; memcpy(e,g_mem+0xb717a4u+0x1cu*k,28); if (e[2]||e[6]) fprintf(stderr," %u:%x+%x/f%u",k,e[2],e[3],e[6]); } putc(10,stderr);
          { uint32_t mg=0,arr=0; memcpy(&mg,g_mem+0xb6bc90u,4); if (mg && mg<DIAG_MEM_SIZE-0x100) { memcpy(&arr,g_mem+mg+0x30u,4); fprintf(stderr,"[bench] blocks:"); if (arr && arr<DIAG_MEM_SIZE-48*0x28) for (unsigned k=0;k<48;k++) { uint32_t e[10]; memcpy(e,g_mem+arr+0x28u*k,40); if (e[0]) fprintf(stderr," [%x sz=%x at=%x st=%x/%x w4=%x w5=%x w7=%x w8=%x w9=%x f=%u]",e[0],e[1],e[6],e[2],e[3],e[4],e[5],e[7],e[8],e[9],(unsigned)g_mem[arr+0x28u*k+0x25u]); } putc(10,stderr); } } }
          com_resource_report();
          fprintf(stderr,"[bench] ready ok after %llums (stage=%u room=%u state=%u)\n",(unsigned long long)el,g_mem[0x70e66d9],ord,cur); done=1; }
        else if (el>=max && getenv("SH3_BENCH_KEEP")) { static unsigned once; if (!once++) { if (getenv("SH3_BENCH_DUMPMAP")) { fputs("[bench] dumping the game map table (F_0049cb80)",stderr); putc(10,stderr); { uint32_t b0=0,o0=0,o1=0; memcpy(&b0,g_mem+0xbb8044u,4); memcpy(&o0,g_mem+0xbb8048u,4); rt_guest_call(c,0x49cb80u,0); memcpy(&o1,g_mem+0xbb8048u,4);
              fprintf(stderr,"[bench] debug buffer base=%08x off %u -> %u",b0,o0,o1); putc(10,stderr);
              if (b0 && b0<DIAG_MEM_SIZE-65536) { uint32_t lo=o1>o0?o0:0, n=o1>lo?o1-lo:0; if (n>8000) n=8000; char *t=malloc(n+1); if (t) { memcpy(t,g_mem+b0+lo,n); t[n]=0; fputs(t,stderr); putc(10,stderr); free(t); } } } } fprintf(stderr,"[bench] READY TIMEOUT (kept alive for debugging) stage=%u state=%u\n",g_mem[0x70e66d9],cur); fflush(stderr); } }
        else if (el>=max) { fprintf(stderr,"[bench] READY TIMEOUT after %llums stage=%u room=%u state=%u (wanted %u:%u); ending failed bench (no automatic restart)\n",(unsigned long long)el,g_mem[0x70e66d9],ord,cur,bench_want_stage,bench_want_room); bench_progress("timeout",bench_i); fflush(stderr); _exit(88); }
    }
    else if (!strcmp(s->cmd,"file")) {
        char guest[300],resolved[4096]; struct stat st; snprintf(guest,sizeof guest,"C:\\SILENT HILL 3\\%s",s->arg);
        int r=diag_data_path(guest,resolved,sizeof resolved);
        if (r==1 && !stat(resolved,&st)) fprintf(stderr,"[bench] file OK %s size=%llu\n",s->arg,(unsigned long long)st.st_size);
        else fprintf(stderr,"[bench] file MISSING %s (r=%d)\n",s->arg,r);
        done=1;
    }
    else if (!strcmp(s->cmd,"movie")) {
        if (!bench_started_movie) { bench_started_movie=1; unsigned idx=(unsigned)atoi(s->arg); fprintf(stderr,"[bench] movie trigger index=%u (F_004bafd0)\n",idx); rt_guest_call(c,0x4bafd0u,3,idx,0u,0u); }
        done=1; bench_started_movie=0;
    }
    else if (!strcmp(s->cmd,"microbench")) {   /* cost per call of translated hot leaves vs a native float equivalent (in game, D3DX table ready) */
        extern void F_0062a874(CPU *); extern void F_0048c290(CPU *);
        uint8_t *const mb=g_mem; CPU save=*c; uint32_t b=(c->esp-0x40000u)&~15u, v=b, m=b+0x40, o=b+0x100, p=b+0x140, sp=b+0x8000; const int N=200000;
        for (int k=0;k<16;k++) { float f=(float)(k*0.37-2.1); memcpy(g_mem+m+4*k,&f,4); } { float f[3]={1.5f,-2.25f,3.125f}; memcpy(g_mem+v,f,12); float q[3]={0.5f,0.75f,-900.f}; memcpy(g_mem+p,q,12); }
        struct timespec t0,t1,t2,t3; volatile float sink=0;
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for (int i=0;i<N;i++) { c->esp=sp; W32(sp,0); W32(sp+4,o); W32(sp+8,v); W32(sp+12,m); F_0062a874(c); }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        for (int i=0;i<N;i++) { const float *vf=(const float *)(g_mem+v), *mf=(const float *)(g_mem+m); float *of=(float *)(g_mem+o);
            float x=vf[0]+(float)(i&1); for (int j=0;j<4;j++) of[j]=((x*mf[j]+vf[1]*mf[4+j])+vf[2]*mf[8+j])+mf[12+j]; sink+=of[3]; }
        clock_gettime(CLOCK_MONOTONIC,&t2);
        for (int i=0;i<N;i++) { c->esp=sp; W32(sp,0); W32(sp+4,o); W32(sp+8,v); W32(sp+12,p); F_0048c290(c); }
        clock_gettime(CLOCK_MONOTONIC,&t3);
        #define MB_NS(a,b) ((double)((b).tv_sec-(a).tv_sec)*1e9+(double)((b).tv_nsec-(a).tv_nsec))
        fprintf(stderr,"[microbench] vec3 via D3DX dispatch %.1f ns/call, native float %.1f ns/call, clip translated %.1f ns/call (N=%d)\n",
                MB_NS(t0,t1)/N,MB_NS(t1,t2)/N,MB_NS(t2,t3)/N,N);
        *c=save; done=1;
    }
    else if (!strcmp(s->cmd,"profile")) {   /* profile N: sampled guest profile over the next N frames (SH3_PROFILE builds only) */
#ifdef SH3_PROFILE
        { extern unsigned g_frame; void rt_prof_window(unsigned,unsigned); unsigned n=(unsigned)atoi(s->arg); rt_prof_window(g_frame,g_frame+(n?n:1200)); }
#else
        fprintf(stderr,"[bench] profile ignored: not an SH3_PROFILE build\n");
#endif
        done=1;
    }
    else if (!strcmp(s->cmd,"wait_room")) {
        unsigned stage=0,room=0,max=120000,ordinal=0;
        if(sscanf(s->arg,"%u %u %u",&stage,&room,&max)<2 || stage>39 || room>266){fprintf(stderr,"[bench] invalid wait_room\n");fflush(stderr);_exit(89);}
        memcpy(&ordinal,g_mem+0x897e48u,4);
        if(g_mem[0x70e66d9]==stage && ordinal==room && bench_state_top()==5)bench_stable++;else bench_stable=0;
        if(bench_stable>=45){fprintf(stderr,"[bench] observed room %u:%u after %llums\n",stage,room,(unsigned long long)el);done=1;}
        else if(el>=max){fprintf(stderr,"[bench] WAIT_STATE TIMEOUT room want=%u:%u cur=%u:%u after %llums\n",stage,room,g_mem[0x70e66d9],ordinal,(unsigned long long)el);bench_progress("timeout",bench_i);fflush(stderr);_exit(88);}
    }
    else if (!strcmp(s->cmd,"wait_stack")) {
        unsigned index=0,want=0,max=30000,value=0;
        if(sscanf(s->arg,"%u %u %u",&index,&want,&max)<2 || index>=5){fprintf(stderr,"[bench] invalid wait_stack\n");fflush(stderr);_exit(89);}
        memcpy(&value,g_mem+0xbcc65cu+4*index,4);
        if(value==want)bench_stable++;else bench_stable=0;
        if(bench_stable>=30){fprintf(stderr,"[bench] stack[%u]=%u reached after %llums\n",index,want,(unsigned long long)el);done=1;}
        else if(el>=max){fprintf(stderr,"[bench] WAIT_STATE TIMEOUT stack[%u] want=%u cur=%u after %llums\n",index,want,value,(unsigned long long)el);bench_progress("timeout",bench_i);fflush(stderr);_exit(88);}
    }
    else if (!strcmp(s->cmd,"wait_state")) {
        unsigned want=0,max=60000; sscanf(s->arg,"%u %u",&want,&max); unsigned cur=bench_state_top();
        if (cur==want) bench_stable++; else bench_stable=0;
        if (bench_stable>=30) { fprintf(stderr,"[bench] state %u reached after %llums\n",want,(unsigned long long)el); done=1; }
        else if (el>=max) { fprintf(stderr,"[bench] WAIT_STATE TIMEOUT want=%u cur=%u after %llums\n",want,cur,(unsigned long long)el); bench_progress("timeout",bench_i); fflush(stderr); _exit(88); }
    }
    else if (!strcmp(s->cmd,"shot")) { char p[256]; snprintf(p,sizeof p,"%s/%s",bench_dir(),s->arg); diag_bench_shot(p); done=1; }
    else if (!strcmp(s->cmd,"shots")) {
        char name[96]; unsigned cnt=1,iv=1000; if (sscanf(s->arg,"%95s %u %u",name,&cnt,&iv)<1) done=1;
        else if (bench_series_done<cnt && el>=(uint64_t)bench_series_done*iv) { char p[256]; snprintf(p,sizeof p,"%s/%s-%02u",bench_dir(),name,bench_series_done); diag_bench_shot(p); bench_series_done++; }
        if (bench_series_done>=cnt) done=1;
    }
    else { fprintf(stderr,"[bench] step %d unknown command %s\n",bench_i,s->cmd); done=1; }
    if (done) { bench_status("done",bench_i); bench_progress("done",bench_i); bench_i++; bench_started=0; if (bench_resume && bench_i==3) { bench_i=bench_resume; bench_resume=0; fprintf(stderr,"[bench] resuming at step %d\n",bench_i); } }
}

/* Game debug text sink (F_004df2c0: vsprintf into a buffer at [0xbb8044]+[0xbb8048]); mirrors the map memory manager dump
   (MapID/ADDR/SIZE/Pr/Rate/L and "remove:" lines) to the journal. Windows: SH3_DIAG_MAPLOG=1. Vita: marker maplog. */
extern void __real_F_004df2c0(CPU *);
void __wrap_F_004df2c0(CPU *c) {
    static int on=-1; if (on<0) {
#ifdef _WIN32
        on=getenv("SH3_DIAG_MAPLOG")!=NULL;
#else
        FILE *mf=fopen("ux0:data/sh3/diag/maplog","rb"); on=mf!=NULL; if (mf) fclose(mf);
#endif
    }
    if (!on) { __real_F_004df2c0(c); return; }
    uint32_t base=0,off0=0,off1=0; memcpy(&off0,g_mem+0xbb8044u,4); memcpy(&base,g_mem+0xbb8048u,4);
    __real_F_004df2c0(c);
    memcpy(&off1,g_mem+0xbb8044u,4);
    if (base && off1>off0 && off1-off0<1024 && base<DIAG_MEM_SIZE-2048) {
        char t[1100]; memcpy(t,g_mem+base+off0,off1-off0); t[off1-off0]=0;
        fputs("[maplog] ",stderr); fputs(t,stderr); if (t[off1-off0-1]!=10) putc(10,stderr);
    }
}

/* Stream job trace (Windows SH3_DIAG_IOTRACE=1): every job post of the game's stream manager (F_00600270, request in EDI)
   and every change of the two I/O slots' transferred counts. */
extern void __real_F_00600270(CPU *);
void __wrap_F_00600270(CPU *c) {
    static int on=-1; if (on<0) on=getenv("SH3_DIAG_IOTRACE")!=NULL;
    if (!on) { __real_F_00600270(c); return; }
    uint32_t r=c->edi, f[10]={0}; if (r && r<DIAG_MEM_SIZE-40) memcpy(f,g_mem+r,40);
    uint32_t idx=0,b0=0,b1=0,n0=0,n1=0; memcpy(&idx,g_mem+0x70e63d4u,4); memcpy(&b0,g_mem+0x70e63a4u,4); memcpy(&b1,g_mem+0x70e63c8u,4); memcpy(&n0,g_mem+0x70e63acu,4); memcpy(&n1,g_mem+0x70e63d0u,4);
    __real_F_00600270(c);
    fprintf(stderr,"[io] post req=%08x id=%x size=%x cap=%x loaded=%x -> ret=%u idx=%u busy=%u/%u n=%x/%x",r,f[0],f[1],f[7],f[8],c->eax,idx,b0,b1,n0,n1); { uint32_t sl[6]; memcpy(sl,g_mem+0x70e638cu+36u*idx,24); fprintf(stderr," slot fn=%08x a=%x id=%x off=%x len=%x dst=%x",sl[0],sl[1],sl[2],sl[3],sl[4],sl[5]); } putc(10,stderr);
}

/* Map directory cache lookup trace (SH3_DIAG_IOTRACE=1): F_0049c230(EAX=map id) returns node+8 or 0; logs hit/miss and the
   node's first words so a node evicted or reinitialised under an in-flight request shows up. */
extern void __real_F_0049c230(CPU *);
void __wrap_F_0049c230(CPU *c) {
    static int on=-1; if (on<0) on=getenv("SH3_DIAG_IOTRACE")!=NULL;
    if (!on) { __real_F_0049c230(c); return; }
    uint32_t id=c->eax, n=0, hit=0, steps=0; memcpy(&n,g_mem+0xb6bc9cu,4);
    while (n && n<DIAG_MEM_SIZE-16 && steps++<80) { uint32_t v=0,nx=0; memcpy(&v,g_mem+n+8,4); if (v==id) { hit=n; break; } memcpy(&nx,g_mem+n,4); n=nx; }
    uint32_t tail=0; memcpy(&tail,g_mem+0xb6bca0u,4);
    __real_F_0049c230(c);
    uint32_t r=c->eax, w[16]={0}; if (r && r<DIAG_MEM_SIZE-64) memcpy(w,g_mem+r,64);
    fprintf(stderr,"[dir] id=%x %s node=%08x tail=%08x ->",id,hit?"hit":"MISS",hit,tail);
    for (unsigned k=0;k<16;k++) fprintf(stderr," %x",w[k]);
    putc(10,stderr);
}

/* Cross-unit archive handle/seek tracing; no guest-state changes. */
#ifdef _WIN32
extern void __real_F_00605b80(CPU *);
extern void __real_F_00605ef0(CPU *);
static int bench_io_trace(void) {
    static int enabled=-1;
    if (enabled<0) enabled=getenv("SH3_DIAG_IOTRACE")!=NULL;
    return enabled;
}
void __wrap_F_00605b80(CPU *c) {
    uint32_t sp=c->esp, path=0;
    if (bench_io_trace() && sp<DIAG_MEM_SIZE-16) memcpy(&path,g_mem+sp+4,4);
    __real_F_00605b80(c);
    if (bench_io_trace() && path && path<DIAG_MEM_SIZE-1024)
        fprintf(stderr,"[archive-open] name=%.300s handle=%x\n",g_mem+path,c->eax);
}
void __wrap_F_00605ef0(CPU *c) {
    uint32_t args[3]={0};
    if (bench_io_trace() && c->esp<DIAG_MEM_SIZE-16) memcpy(args,g_mem+c->esp+4,12);
    __real_F_00605ef0(c);
    if (bench_io_trace())
        fprintf(stderr,"[archive-seek] handle=%x offset=%x origin=%x returned=%x\n",args[0],args[1],args[2],c->eax);
}
#endif
