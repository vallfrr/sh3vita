"""Isolate PC24 Vec3 with retained fallback; original generated files unchanged."""
import argparse,hashlib,re
from pathlib import Path
SIGNATURE='bfe7c418f2aa0f793c36bd31da9ca8355e043975f358a461a84568284b9a83ba'
def transform(source):
    m=re.search(r'void F_006483a7\(CPU \*c\) \{.*?\n\}',source,re.S)
    if not m or hashlib.sha256(m[0].encode()).hexdigest()!=SIGNATURE:
        raise ValueError('Vec3 source changed: re-audit')
    retained=m[0].replace('void F_006483a7(', 'static void pc24_vec3_retained(')
    wrapper='''void F_006483a7(CPU *c) {
  if (g_diag_pc24_vec3 && diag_pc24_vec3(c)) {
    static unsigned announced;
    if (!__atomic_load_n(&announced,__ATOMIC_RELAXED)) {
      unsigned expected=0;
      if (__atomic_compare_exchange_n(&announced,&expected,1,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED))
        rt_log("[pc24-vec3] first bounded native PC24 path executed\\n");
    }
    return;
  }
  pc24_vec3_retained(c);
}'''
    return '#include "pc24_vec3.h"\nextern int g_diag_pc24_vec3;\nextern void rt_log(const char *,...);\n'+source[:m.start()]+retained+'\n'+wrapper+source[m.end():]
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):raise SystemExit('Isolated output required')
    result=transform(a.source.read_text());a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(result)
