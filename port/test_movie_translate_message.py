#!/usr/bin/env python3
"""Check the real bounded TranslateMessage movie-notification implementation."""
import os
from pathlib import Path
import subprocess
import tempfile
s=(Path(__file__).resolve().parent/'diag_runtime.c').read_text(encoding='utf-8')
a=s.index('void imp_USER32_TranslateMessage(');b=s.index('\nvoid imp_USER32_DispatchMessageA',a)
shim=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SH3_MOVIE_LAB 1
#define DIAG_MEM_SIZE 64u
#define DIAG_FLAG(name) ({ static int _df=-1; if (_df<0) _df=getenv(name)!=NULL; _df; })
typedef struct { uint32_t esp,eax; } CPU;
typedef struct { uint32_t hwnd,message,wparam,lparam,time,x,y; } DiagMessage;
static uint8_t memory[64],*g_mem=memory;
static CPU cpu,*diag_main_cpu=&cpu;
'''
test=r'''
int main(int argc,char **argv) {
 (void)argv; setenv("SH3_DIAG_MSG_QUEUE","1",1); cpu.esp=0; cpu.eax=123;
 uint32_t p=16; memcpy(memory+4,&p,4); DiagMessage m={.hwnd=0x10010,.message=0x40d};
 if(argc>1) m.message=0x100; memcpy(memory+p,&m,sizeof m);
 imp_USER32_TranslateMessage(&cpu);
 if(argc>1) return 1;
 return cpu.eax==0 && cpu.esp==8 ? 0 : 2;
}
'''
with tempfile.TemporaryDirectory(prefix='sh3-movie-translate-') as t:
 p=Path(t);(p/'test.c').write_text(shim+s[a:b]+test,encoding='utf-8')
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 r=subprocess.run([str(p/'test'),'keyboard'],capture_output=True)
 assert r.returncode==30,r.returncode
 print('movie TranslateMessage gate passed')
