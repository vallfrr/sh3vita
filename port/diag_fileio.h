#ifndef SH3_DIAG_FILEIO_H
#define SH3_DIAG_FILEIO_H
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* Only for read-only streams whose reads all use this function. pread avoids
   stdio's small transfers; fseeko maintains the guest-visible stream offset. */
static int diag_direct_read(FILE *f, void *buffer, size_t size, size_t *done) {
    off_t pos=ftello(f);
    if (pos<0) return -1;
    uintmax_t max_pos=((uintmax_t)1 << (sizeof(off_t)*8-1))-1;
    if ((uintmax_t)pos>max_pos || (uintmax_t)size>max_pos-(uintmax_t)pos) { errno=EOVERFLOW; return -1; }
    *done=0;
    while (*done<size) {
        size_t request=size-*done;
        if (request>0x7fffffffu) request=0x7fffffffu;
        ssize_t n=pread(fileno(f),(char *)buffer+*done,request,pos+(off_t)*done);
        if (n<0 && errno==EINTR) continue;
        if (n<0) return -1;
        if (!n) break;
        *done+=(size_t)n;
    }
    return fseeko(f,pos+(off_t)*done,SEEK_SET);
}
#endif
