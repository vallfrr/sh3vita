#define _POSIX_C_SOURCE 200809L
#include "diag_fileio.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
int main(int argc, char **argv) {
    FILE *w=tmpfile(); assert(w);
    const char data[]="0123456789abcdef";
    assert(fwrite(data,1,16,w)==16 && !fflush(w));
    FILE *r=fdopen(dup(fileno(w)),"rb"); assert(r);
    assert(!fseeko(r,0,SEEK_SET));
    char out[32]={0}; size_t n;
    assert(!diag_direct_read(r,out,5,&n) && n==5 && !memcmp(out,"01234",5));
    assert(ftello(r)==5);
    assert(!diag_direct_read(r,out,4,&n) && n==4 && !memcmp(out,"5678",4));
    assert(!fseeko(r,12,SEEK_SET));
    assert(!diag_direct_read(r,out,16,&n) && n==4 && !memcmp(out,"cdef",4));
    assert(ftello(r)==16);
    assert(!diag_direct_read(r,out,1,&n) && n==0 && ftello(r)==16);
    assert(!fseeko(r,2,SEEK_SET));
    assert(!diag_direct_read(r,out,0,&n) && n==0 && ftello(r)==2);
    fclose(r); fclose(w);
    if (argc>1) {
        FILE *direct=fopen(argv[1],"rb"), *stdio=fopen(argv[1],"rb");
        assert(direct && stdio && !fseeko(stdio,0,SEEK_END));
        off_t length=ftello(stdio); assert(length>0);
        uint32_t seed=0x12345678; unsigned char a[4096],b[4096];
        for (unsigned i=0;i<1000;i++) {
            seed=seed*1664525u+1013904223u; off_t pos=(off_t)((uint64_t)seed%((uint64_t)length+1));
            seed=seed*1664525u+1013904223u; size_t want=seed%sizeof a;
            assert(!fseeko(direct,pos,SEEK_SET) && !fseeko(stdio,pos,SEEK_SET));
            assert(!diag_direct_read(direct,a,want,&n));
            size_t ref=fread(b,1,want,stdio);
            assert(!ferror(stdio) && n==ref && !memcmp(a,b,n) && ftello(direct)==ftello(stdio));
        }
        fclose(direct); fclose(stdio);
    }
    return 0;
}
