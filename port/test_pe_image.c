#include "pe_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
static void put32(uint8_t *p, uint32_t n) { p[0] = n; p[1] = n >> 8; p[2] = n >> 16; p[3] = n >> 24; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(int argc, char **argv) {
    uint8_t f[0x300] = {0}, a[0x40000];
    memset(a, 0xa5, sizeof a);
    put16(f, 0x5a4d); put32(f + 0x3c, 0x80); put32(f + 0x80, 0x4550);
    put16(f + 0x84, 0x14c); put16(f + 0x86, 1); put16(f + 0x94, 0x60);
    size_t opt = 0x98, sec = opt + 0x60;
    put16(f + opt, 0x10b); put32(f + opt + 16, 0x1000); put32(f + opt + 28, 0x10000);
    put32(f + opt + 56, 0x2000); put32(f + opt + 60, 0x200);
    put32(f + sec + 8, 0x1000); put32(f + sec + 12, 0x1000);
    put32(f + sec + 16, 0x100); put32(f + sec + 20, 0x200);
    for (unsigned i = 0; i < 0x100; i++) f[0x200 + i] = (uint8_t)i;
    Sh3PeLayout l = {0};
    CHECK(sh3_pe_load(f, sizeof f, a, sizeof a, &l) == 0);
    CHECK(l.image_base == 0x10000 && l.image_size == 0x2000 && l.entry == 0x11000 && l.heap_base == 0x20000);
    CHECK(!memcmp(a + 0x11000, f + 0x200, 0x100));
    CHECK(a[0x11100] == 0 && a[0x10000 - 1] == 0xa5 && a[0x12000] == 0xa5);
    put32(f + sec + 20, 0x280); /* 0x100 bytes extends past input */
    memset(a, 0x7c, sizeof a); l.entry = 0xdecafbad;
    CHECK(sh3_pe_load(f, sizeof f, a, sizeof a, &l) == -7);
    CHECK(a[0x10000] == 0x7c && l.entry == 0xdecafbad);
    put32(f + sec + 20, 0x200);
    CHECK(sh3_pe_load(f, sizeof f, a, 0x11000, &l) == -4);
    f[0] = 0;
    CHECK(sh3_pe_load(f, sizeof f, a, sizeof a, &l) == -1);

    if (argc == 2) {
        FILE *in = fopen(argv[1], "rb"); CHECK(in);
        CHECK(!fseek(in, 0, SEEK_END)); long n = ftell(in); CHECK(n > 0); rewind(in);
        uint8_t *bytes = malloc((size_t)n), *mem = malloc(200u * 1024 * 1024); CHECK(bytes && mem);
        CHECK(fread(bytes, 1, (size_t)n, in) == (size_t)n); fclose(in);
        CHECK(!sh3_pe_load(bytes, (size_t)n, mem, 200u * 1024 * 1024, &l));
        CHECK(l.image_base == 0x400000 && l.image_size == 0x6d7c0a8 && l.entry == 0x662a6e && l.heap_base == 0x7180000);
        CHECK(!memcmp(mem + 0x401000, bytes + 0x1000, 0x289000));
        CHECK(mem[0x6b0000 + 0x7c000] == 0);
        printf("SH3 PE load: entry=%08x heap=%08x\n", l.entry, l.heap_base);
        free(bytes); free(mem);
    }
    puts("PE image tests passed");
    return 0;
}
