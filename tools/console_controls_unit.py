#!/usr/bin/env python3
"""Adapt native console options in signed copies; generated units stay untouched."""
import argparse, hashlib, re
from pathlib import Path
SIGNATURES = {
 '005d80c0': 'b4fff45d1b79de812514b4e3dad2de8c47b9f13ba9523f7b7eee884eb0981a97',
 '005e2e60': '247339549a3ac8e8f1121b8aaf11b4a802463e763a3a67cb4af6be8bc3b32deb',
 '005e3130': '4833afd411f739fc54e9def1437ac84388238b35dd7187a4b79d054017a9b6aa',
 '005e3e10': '171ea7fb5ab5c618c555180571ec4fc38b845eff3d8841d71b7aa8b5d8f1bac6',
 '005e40f0': '118bf9aaa248e24aa6df67766b22e99551694a9dfb5f1c96478dfd92ee861a33',
 '005e6040': '8f9c5c03d4165a8c51fe28ae7b2d5d7e32f29d7676867f5b917bf218bed8368b',
 '005e62a0': 'da31c154b1da7a3bd4074c7617d9c5412349dd7365fc4b57b414b26e1b1c087f',
 '005e68f0': 'b9732104a5e8a7c95e9e3d2b7ae47f378636f43eaa40a9137a1ff0670b1b4916',
 '005e2300': '339ae7529d486ef24dee79c5349d409c21d0845eef2b08c16af8aea6ce1424e3',
 '005e4310': '8991959e820c2f329741e51dfca2010fcd07d9371da913bc78ed911826187303',
 '005e4e50': '348eabeb3ecd61523bbaad50769aad5e7987238db790d6fe42f8d2a503aedd53',
 '005e4c60': 'ce383ddfcd712ead023093baf2f4d2d1d3f2db6bb565bcc1269ec4a112e2a5c7',
 '0060cc80': '9e723e54c773ab85d48e00bafa4918a119c8be29950a17dfeeb2a5b7b25fb9ff',
 '0060cc90': '2acac29ace4a757b68e451906e7d315f52db8ea7ea2cc216874bed184d3c5178',
 '005e3f90': '1ba46c9f6510ef2adcabb37f370b25b5c72bb6770f4ea366ac8f77f540b5e01b',
 '005e3560': '2320276a5c8c6a2a2c719488e9899d628f4e6cc5df2836c31a032c10773ad5e5',
 '005e8270': '2e289bec861f7e64814b6351b094d7ca48b4bc0135b17b81ab74dc995334aaa0',
 '005e88b0': '4760372a49d4075a74e02fff64a16542b9857f3cbb47ead8a0780a6d3e1baa80',
 '005e35e0': '6985c5a61fce46804fa7f1c16db7dcd066082640af964e61f45c66575dd75d39',
}
PAGE_BODY = """  /* Two pages only (Main <-> Extra): the PC Configuration page is retired, its Gamma row sits on page 1. */
  uint8_t *const mb = g_mem; uint32_t esp=c->esp, cur=R8(0xbcc178u), to=cur, n;
  if(cur>=1u && cur<=10u){ to=cur+10u; if(to>0x15u)to=0x15u; for(n=0;n<11u && !R8(0xbcc182u+to);n++){to++;if(to>0x15u)to=0xbu;} }
  else if(cur>=0xbu && cur<=0x15u){ to=cur-10u; if(to>10u)to=10u; if(to<1u)to=1u; for(n=0;n<10u && !R8(0xbcc182u+to);n++){to++;if(to>10u)to=1u;} }
  else to=1u;
  W8(0xbcc179u,to&0xffu);
  if(cur!=to){ uint32_t e=esp; e-=4;W32(e,0u); e-=4;W32(e,0x3f800000u); e-=4;W32(e,1u); e-=4;W32(e,0x5e3600u); c->esp=e; F_0046cd50(c); }
  c->esp=esp+4u;
"""
def replace(source):
    found = 0
    for name, sha in SIGNATURES.items():
        m = re.search(r'void F_'+name+r'\(CPU \*c\) \{.*?\n\}',source,re.S)
        if not m: continue
        if hashlib.sha256(m[0].encode()).hexdigest()!=sha:
            raise ValueError('Unsupported native controls function '+name)
        body=m[0]
        if name=='005d80c0':
            body=body.replace('{\n','{\n  if(g_mem[0xbcbff8u]==9u)g_mem[0xbcbff8u]=3u; /* Retired title Quit entry. */\n',1)
            needle='  // 0x5d812a: mov dword ptr [esp + ebp*4 + 0x18], 9'
            assert body.count(needle)==1
            body=body.replace(needle,'  goto L_005d8133; /* Exclude Quit from rendering, navigation and hit rectangles. */\n'+needle)
            body=body.replace('  // 0x5d8133: cmp eax, ebx','L_005d8133:;\n  // 0x5d8133: cmp eax, ebx')
        elif name=='005e2e60':
            # All hidden rows must also leave no mouse/hit-test rectangle.
            body=body.replace('L_005e2fad:;', 'L_005e2fad:;\n  goto L_005e2fc4; /* Compact hidden console rows on every page. */')
            body=body.replace('{\n','{\n  { uint8_t *const mb=g_mem; W32(0x71e6b0u+5u*8u,R32(0x71e6b0u+25u*8u));W32(0x71e6b4u+5u*8u,R32(0x71e6b4u+25u*8u)); } /* Gamma row shown in the former Vibration slot. */\n',1)
        elif name=='005e3130':
            needle='  // 0x5e3204: xor eax, eax'
            assert body.count(needle)==1
            body=body.replace(needle, '''  /* The native Extra page already counts visible rows. Apply that same
     layout to Main and Image, without changing font size or message data. */
  if (edi>=11u && edi<=28u) {
    uint32_t first=edi<=10u?1u:(edi<=21u?11u:22u), count=0;
    for(uint32_t row=first;row<edi;row++)count+=R8(0xbcc182u+row)!=0;
    esi=0x60u+25u*count; goto L_005e3262;
  }
'''+needle)
        elif name=='005e3f90':
            assert body.count('0x19u')==5
            body=body.replace('0x19u','0x5u') # Gamma correction handler moved from row 25 to row 5 (page 1).
        elif name in ('005e3560','005e35e0'):
            body='void F_'+name+'(CPU *c) {'+chr(10)+PAGE_BODY+'}'
        elif name in ('005e3e10','005e6040','005e62a0'):
            # PC render-target resolution, display resolution, window mode UI.
            # Their initialization and engine values are deliberately preserved.
            body='void F_'+name+'(CPU *c) { c->esp+=4; }'
        elif name=='005e40f0':
            body=body.replace('{\n','{\n  if(c->esi==26u || c->esi==27u){c->esp+=4;return;} /* PC lens/afterimage switches. */\n',1)
        elif name=='005e68f0':
            body=body.replace('{\n','''{
  /* Keep the real gamma screen and native exit/apply/cancel flow. Internal
     renderer choices stay as loaded; hiding a setting does not disable it. */
  g_mem[0xbcc198u]=g_mem[0xbcc199u]=g_mem[0xbcc19au]=0;
  g_mem[0xbcc19cu]=g_mem[0xbcc19du]=0;  g_mem[0xbcc19bu]=g_mem[0xbcc19eu]=0; /* Gamma now lives on page 1; the Configuration page is retired. */
  if((g_mem[0xbcc178u]>=22u && g_mem[0xbcc178u]<=24u) ||
     g_mem[0xbcc178u]==26u || g_mem[0xbcc178u]==27u)
    g_mem[0xbcc178u]=g_mem[0xbcc179u]=25u;
''',1)
        elif name=='005e88b0':
            # Pause menu: the 4th entry (Inventory, reachable with Triangle anyway) opens Options (state 0xe), like the inventory's own Options button.
            old='  // 0x5e8b78: push 6\n  { uint32_t v=0x6u; esp-=4; W32(esp,v); }'
            assert body.count(old)==1
            body=body.replace(old,'  // 0x5e8b78: push 0xe (Options), return flag F_005e3110: 2 = back to the game on exit (1 = inventory, 0 = title)\n  g_mem[0xbcc17du]=2; sh3_pause_reopen=45; sh3_pause_cursor=3u; { uint32_t v=0xeu; esp-=4; W32(esp,v); }')
        elif name=='005e8270':
            # Per-frame pause opener: back from Options (see 005e88b0), reopen the pause menu with the cursor on Options.
            needle='  // 0x5e828d: call 0x5f1320'
            assert body.count(needle)==1
            body=body.replace(needle,'  if(sh3_pause_reopen && --sh3_pause_reopen==0){ goto L_005e82a4; }\n'+needle)
            old='  W32((uint32_t)(0xbcc210u),0x0u);'
            assert body.count(old)==1
            body=body.replace('  // 0x5e82a4: mov eax, 1','L_005e82a4:;\n  // 0x5e82a4: mov eax, 1')
            body=body.replace(old,'  W32((uint32_t)(0xbcc210u),sh3_pause_cursor); sh3_pause_cursor=0;')
            body='static int sh3_pause_reopen; static uint32_t sh3_pause_cursor;\n'+body
        elif name=='005e2300':
            needle='  // 0x5e24b4: fild dword ptr [esp + 0x28]'
            assert body.count(needle)==1
            body=body.replace(needle, '  if(diag_console_display_mode(R32(esp+0x28u),R32(esp+0x2cu))) goto L_005e24f8;\n'+needle)
            body=body.replace('  // 0x5e24f8:', 'L_005e24f8:;\n  // 0x5e24f8:')
        elif name=='005e4310':
            # Row 5 (former Vibration) is kept visible: Gamma correction takes its place (see 005e3f90).
            old='W8((uint32_t)(0xbcc18bu),(ebx&0xffu));'
            assert body.count(old)==1
            body=body.replace(old,'W8((uint32_t)(0xbcc18bu),0); /* Type3D row excluded from navigation. */')
        elif name=='005e4c60':
            body='void F_005e4c60(CPU *c) { c->esp+=4; }'
        elif name=='005e4e50':
            body='void F_005e4e50(CPU *c) { g_mem[0xbcc1a8u]=0; c->esp+=4; }'
        elif name=='0060cc80':
            body='void F_0060cc80(CPU *c) { g_mem[0x70e66cdu]=0; c->eax&=0xffffff00u; c->esp+=4; }'
        else:
            body='void F_0060cc90(CPU *c) { g_mem[0x70e66cdu]=0; c->eax&=0xffffff00u; c->esp+=4; }'
        source=source[:m.start()]+body+source[m.end():]
        found+=1
    if found not in (1,2,3,6,7,10): raise ValueError('Missing native controls/display functions')
    if 'void F_005e2300(' in source:
        source='#include <stdint.h>\n#include <stdlib.h>\nextern int diag_console_display_mode(uint32_t w,uint32_t h);\n'+source
    return source
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):
        raise SystemExit('Output must be outside generated directory')
    result=replace(a.source.read_text(encoding='utf-8'))
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(result,encoding='utf-8')
