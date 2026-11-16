/* Vita-only path translation, kept separate from the Mac diagnostic sandbox. */
#include "vita_paths.h"
#include <string.h>
#include <strings.h>

int sh3_vita_data_path(const char *guest, char *out, size_t cap) {
    static const char prefix[]="C:\\SILENT HILL 3\\data";
    static const char root[]="ux0:data/sh3/data";
    if (!guest || !out || strncasecmp(guest,prefix,sizeof prefix-1)) return -1;
    const char *p=guest+sizeof prefix-1;
    if (*p && *p!='/' && *p!='\\') return -1;
    size_t n=sizeof root-1;
    if (cap<=n) return -1;
    memcpy(out,root,n);
    while (*p) {
        if (*p!='/' && *p!='\\') return -1;
        p++; const char *begin=p;
        while (*p && *p!='/' && *p!='\\') {
            unsigned char c=(unsigned char)*p;
            if (c<32 || c>=127 || c==':' || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|') return -1;
            p++;
        }
        size_t len=(size_t)(p-begin);
        if (!len || (len==1 && begin[0]=='.') || (len==2 && begin[0]=='.' && begin[1]=='.') || n+1+len>=cap) return -1;
        out[n++]='/'; memcpy(out+n,begin,len); n+=len;
    }
    out[n]=0; return 0;
}
