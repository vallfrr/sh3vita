#!/usr/bin/env python3
"""Adapt native console options in signed copies; generated units stay untouched."""
import argparse, hashlib, re
from pathlib import Path
SIGNATURES = {
 '005e3400': '8d92be9eba0516f78c18cae56594996fe8273c9ad402b29e516084d237d7344c',
 '005e3480': '32e3339eaa28d2cffe31e973bba74c0ea92376f4aedab77236868e13a1795bac',
 '005e3500': '899f8c41962155c8a5e84383c12c6c7e76a32ba207fa6e1e7572934196e2212f',
 '005e4fc0': 'cdd9bc41f23e23c3319359d52a02c3306666765b51555eeb2cc0313e87f82cd7',
 '005e5130': '71c69907c78b9b23e55b9b24fe466d5debd6019fe2177e4fc42d94bd86f3ef26',
 '005e52a0': 'c4dcd87f7e33a3d45b97c327d46a078f967b11e49683319ca1828fbd780e5b1b',
 '005e53c0': '7d8d6d8640e4e7e3b440d25ee1d85c34a7ed86f85357484086b45d41500f7460',
 '005e54e0': '04008d3e62ef30a2a79e30a0cb24b91f250e355dd091775090b4626c4109f87b',
 '005e5600': 'd8d0259c7da0191248265d532727aa201b118670c2ecd66874351dc9e17ab4b1',
 '005e5820': '390a083053e0893b9a50c5c9eb6a384f78f3f9b6d9154d049cf087e047049410',
 '005e5990': '6ad44f8a65d8b49d2dfa948711eb66ade96cfd68d5b92643e219dacaa9ab9677',
 '005e5ac0': '90b578763f73ea022e8946515822b8473e193083c4616794bb5563d441a1f080',
 '005e3660': '8064d8f72e5a38f7e2c5d46b0072a6f21d0006d83fd4e46d589c52c64118f257',
 '005e4640': 'e768e9448837439d72a3c0fbc64fc081a88b767171f8eb53cf5c04d7d68fde3b',
 '005e4f20': '8ddb7741778f91a843d6c0f612586dcaac00ba6a8a1255a5301201a5d3e43b7b',

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
 '0060d190': '4cbe4a4f3b8c4a06c8b354a8b65ec5af8bf3c7a2b0dc43366856769329b9de10',
 '005e8270': '2e289bec861f7e64814b6351b094d7ca48b4bc0135b17b81ab74dc995334aaa0',
 '005e88b0': '4760372a49d4075a74e02fff64a16542b9857f3cbb47ead8a0780a6d3e1baa80',
 '0058edb0': '949d68c34a7a645ab22b39c9ffc5fedb7f98f9a1d5a0b383f37f935f4e7397b7',
 '005e35e0': '6985c5a61fce46804fa7f1c16db7dcd066082640af964e61f45c66575dd75d39',
}

PAGE_BODY = """  /* Two pages only (Main <-> Extra): the PC Configuration page is retired, movement sits before Extra Exit. */
  uint8_t *const mb = g_mem; uint32_t esp=c->esp, cur=R8(0xbcc178u), to=cur, n;
  if(cur>=1u && cur<=10u){ to=cur+10u; if(to>0x16u)to=0x16u; for(n=0;n<12u && !R8(0xbcc182u+to);n++){to++;if(to>0x16u)to=0xbu;} }
  else if(cur>=0xbu && cur<=0x16u){ to=cur-10u; if(to>10u)to=10u; if(to<1u)to=1u; for(n=0;n<10u && !R8(0xbcc182u+to);n++){to++;if(to>10u)to=1u;} }
  else to=1u;
  W8(0xbcc179u,to&0xffu);
  if(cur!=to){ uint32_t e=esp; e-=4;W32(e,0u); e-=4;W32(e,0x3f800000u); e-=4;W32(e,1u); e-=4;W32(e,0x5e3600u); c->esp=e; F_0046cd50(c); }
  c->esp=esp+4u;
"""
def replace(source):
    original_source=source
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
    uint32_t first=edi<=10u?1u:(edi<=22u?11u:23u), count=0;
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
  g_mem[0xbcc198u]=1; g_mem[0xbcc199u]=g_mem[0xbcc19au]=0; /* Extra Exit now row22; movement row21. */
  g_mem[0xbcc19cu]=g_mem[0xbcc19du]=0;  g_mem[0xbcc19bu]=0; g_mem[0xbcc19eu]=0; /* Gamma now lives on page 1; Configuration remains retired. */
  if((g_mem[0xbcc178u]>=23u && g_mem[0xbcc178u]<=28u) ||
     g_mem[0xbcc178u]==26u || g_mem[0xbcc178u]==27u)
    g_mem[0xbcc178u]=g_mem[0xbcc179u]=21u;
''',1)
        elif name=='005e88b0':
            # Pause menu: the 4th entry (Inventory, reachable with Triangle anyway) opens Options (state 0xe), like the inventory's own Options button.
            old='  // 0x5e8b78: push 6\n  { uint32_t v=0x6u; esp-=4; W32(esp,v); }'
            assert body.count(old)==1
            body=body.replace(old,'  // 0x5e8b78: push 0xe (Options), return flag F_005e3110: 2 = back to the game on exit (1 = inventory, 0 = title)\n  g_mem[0xbcc17du]=2; sh3_pause_reopen=45; sh3_pause_cursor=3u; { uint32_t v=0xeu; esp-=4; W32(esp,v); }')
        elif name=='0058edb0':
            # Options left from the pause menu (return flag 2, see 005e88b0): take the native in-game path (flag 1, the
            # inventory's Options) so changed settings are written to data.sys (system save, state 0x13, return target 4),
            # but come back to the game instead of the inventory (state 6), through the return-to-game transition (state 7,
            # F_005e1e20, also used when a menu closes): it reloads the room's message file, which Options replaces by its own
            # in the same buffer (0xb577f0). Going straight to state 5 kept the Options texts: every prompt then showed
            # key-configuration labels ("Turn Right", "Move Forward"), issue #1. Before, flag 2 went straight to
            # state 5 and the settings (Brightness...) were lost at the next boot (Valentin, 2026-11-16).
            old='  if ((fa!=fb)) goto L_0058f504;\n'
            assert body.count(old)==1
            body=body.replace(old,'  sh3_options_to_game=(fa==fb); goto L_0058f504; /* flag 2 = flag 1 + return to the game; set on every Options exit */\n')
            old='L_0058f486:;\n  // 0x58f486: push 6\n  { uint32_t v=0x6u; esp-=4; W32(esp,v); }'
            assert body.count(old)==1
            body=body.replace(old,'L_0058f486:;\n  // 0x58f486: push 6 (7 = return-to-game transition when leaving pause Options unchanged)\n  { uint32_t v=sh3_options_to_game?0x7u:0x6u; sh3_options_to_game=0; esp-=4; W32(esp,v); }')
            old='  // 0x58f94e: mov esi, 6\n  esi=(uint32_t)(0x6u);'
            assert body.count(old)==1
            body=body.replace(old,'  // 0x58f94e: mov esi, 6 (7 after the system save of pause Options)\n  esi=sh3_options_to_game?0x7u:0x6u; /* re-run each frame until the save ends: no reset here */')
            body='static int sh3_options_to_game;\n'+body
        elif name=='0060d190':
            # Default settings of a new configuration: the film-grain effect ("Bruitage", Options Extra, byte 0x70e66cf)
            # starts OFF (Valentin, 2026-11-16). An existing configuration keeps its saved value.
            old='  W8((uint32_t)(0x70e66cfu),0x1u);'
            assert body.count(old)==1
            body=body.replace(old,'  W8((uint32_t)(0x70e66cfu),0x0u); /* film grain off by default */')
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
            # Row 9 (former control type) stays visible: it is the frame-rate setting (0 = 60, 1 = 30, 2 = unlimited).
            needle='  W8((uint32_t)(0xbcc1a8u),(eax&0xffu));'
            assert body.count(needle)==1
            body=body.replace(needle,needle+'\n  sh3_movement_edit=(g_mem[0x70e66cdu]&0x80u)?0u:1u;')
        elif name=='005e4c60':
            # Reuse the signed native two-value movement widget in the Extra row21 before Exit.
            original=re.search(r'void F_005e4e50\(CPU \*c\) \{.*?\n\}',original_source,re.S)[0]
            assert hashlib.sha256(original.encode()).hexdigest()==SIGNATURES['005e4e50']
            body=original.replace('void F_005e4e50(', 'void F_005e4c60(')
            body=body.replace('0x9u','0x15u')
            body=body.replace('R8((uint32_t)(0xbcc1a8u))','sh3_movement_edit')
            body=body.replace('W8((uint32_t)(0xbcc1a8u),(eax&0xffu));','sh3_movement_edit=eax&1u;')
            body=body.replace('F_0060cc90(c);','c->eax=(c->eax&0xffffff00u)|((g_mem[0x70e66cdu]&0x80u)?0u:1u); c->esp+=4u;')
            # Only the FPS widget owns dirty bit9; merge the independent movement edit afterwards there.
            body=body.replace('W32((uint32_t)(0xbcc1dcu),eax);','/* dirty bit merged by FPS widget */')
            body=body.replace('{\n','{\n  sh3_movement_drawing=1;\n',1)
            body=body.replace('  esp+=4u; c->eax=', '  sh3_movement_drawing=0;\n  esp+=4u; c->eax=')
        elif name=='005e4e50':
            # Row 9 cycles 60 -> 30 -> unlimited (value+1 modulo 3) instead of toggling two values.
            old='  { uint32_t r=(eax&0xffu)&0x1u; fr=r; cf=0; of=0; zf=(r==0);sf=(r>>7)&1u;pf=PF(r); eax=(eax&0xffffff00u)|((uint32_t)(r)&0xffu); }'
            assert body.count(old)==1
            body=body.replace(old,'  { uint32_t a=eax&0xffu; uint32_t r=(((a+1u)&0xffu)+1u)%3u; fr=r; cf=0; of=0; zf=(r==0);sf=0;pf=PF(r); eax=(eax&0xffffff00u)|r; } /* frame-rate setting: 3 values */')
            body=body.replace('  esp+=4u; c->eax=', '  if(sh3_movement_edit!=((g_mem[0x70e66cdu]&0x80u)?0u:1u)) { uint32_t dirty=R32(0xbcc1dcu); W32(0xbcc1dcu,dirty|0x200u); }\n  esp+=4u; c->eax=')
        elif name=='0060cc80':
            # Saved control byte: FPS in bits0..6, movement3D in bit7; native system-save path.
            body='void F_0060cc80(CPU *c) { g_mem[0x70e66cdu]=(g_mem[c->esp+4u]&0x7fu)|(sh3_movement_edit?0u:0x80u); c->eax=(c->eax&0xffffff00u)|g_mem[c->esp+4u]; c->esp+=4; }'
        elif name in ('005e3400','005e3480','005e3500','005e3660','005e4640','005e4f20','005e4fc0','005e5130',
                      '005e52a0','005e53c0','005e54e0','005e5600','005e5820','005e5990','005e5ac0'):
            pass # Native page-range changes are applied below.
        else:
            # Options callers see FPS bits0..6; gameplay sees the independent native movement type.
            # Existing saved FPS0/1/2 retain the previous forced2D behavior; bit7 opts into3D.
            body=('void F_0060cc90(CPU *c) { uint32_t ret=g_mem[c->esp]|g_mem[c->esp+1u]<<8|g_mem[c->esp+2u]<<16|(uint32_t)g_mem[c->esp+3u]<<24;\n'
                  '  c->eax=(c->eax&0xffffff00u)|(ret>=0x5e4000u && ret<0x5e6000u ? (g_mem[0x70e66cdu]&0x7fu) : ((g_mem[0x70e66cdu]&0x80u)?0u:1u)); c->esp+=4; }')
        # Extra grows from11..21 to11..22: keep all native options11..20,
        # movement21 and native Exit22. No third page remains reachable.
        # 005e3500, 005e4fc0 and 005e5130 also derive "which page is this row on" (row<=10 main, <=0x15 extra, else the retired page):
        # left on 0x15, the Extra Exit row (now 0x16) counted as the retired page and the rows above it were not drawn.
        if name in ('005e2e60','005e3130','005e3400','005e3480','005e3500','005e3660','005e4640','005e4f20','005e4fc0','005e5130',
                    '005e52a0','005e53c0','005e54e0','005e5600','005e5820','005e5990','005e5ac0','005e68f0'):
            body=body.replace('0x15u','0x16u')
        source=source[:m.start()]+body+source[m.end():]
        found+=1
    expected=sum(('void F_'+fn+'(CPU *c) {') in original_source for fn in SIGNATURES)
    if not found or found!=expected: raise ValueError('Missing native controls/display functions')
    if 'void F_005e2300(' in source:
        source='#include <stdint.h>\n#include <stdlib.h>\nextern int diag_console_display_mode(uint32_t w,uint32_t h);\n'+source
    return 'extern unsigned sh3_movement_edit,sh3_movement_drawing;\n'+source
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):
        raise SystemExit('Output must be outside generated directory')
    result=replace(a.source.read_text(encoding='utf-8'))
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(result,encoding='utf-8')
