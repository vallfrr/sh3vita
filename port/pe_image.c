#include "pe_image.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int within(size_t off, size_t len, size_t total) { return off <= total && len <= total - off; }

int sh3_pe_load(const uint8_t *file, size_t file_size, uint8_t *arena,
                size_t arena_size, Sh3PeLayout *layout) {
    if (!file || !arena || !layout || !within(0, 64, file_size) || le16(file) != 0x5a4d) return -1;
    size_t pe = le32(file + 0x3c);
    if (!within(pe, 24, file_size) || le32(file + pe) != 0x00004550 || le16(file + pe + 4) != 0x14c) return -2;
    unsigned nsec = le16(file + pe + 6), optlen = le16(file + pe + 20);
    size_t opt = pe + 24;
    if (!nsec || nsec > 96 || optlen < 64 || !within(opt, optlen, file_size) || le16(file + opt) != 0x10b) return -3;
    uint32_t base = le32(file + opt + 28), size = le32(file + opt + 56);
    uint32_t entry_rva = le32(file + opt + 16), headers = le32(file + opt + 60);
    if (!size || !headers || headers > size || entry_rva >= size ||
        !within(0, headers, file_size) || !within(base, size, arena_size)) return -4;
    uint64_t end = (uint64_t)base + size, heap = (end + 0xffffu) & ~(uint64_t)0xffffu;
    if (end > UINT32_MAX || heap >= arena_size || heap > UINT32_MAX) return -5;
    size_t sections = opt + optlen;
    if (!within(sections, (size_t)nsec * 40, file_size)) return -6;
    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t *s = file + sections + i * 40u;
        uint32_t vsz = le32(s + 8), va = le32(s + 12), rsz = le32(s + 16), raw = le32(s + 20);
        uint32_t copy = !vsz || rsz < vsz ? rsz : vsz;
        if (!within(va, vsz, size) || !within(va, copy, size) || !within(raw, copy, file_size)) return -7;
    }
    memset(arena + base, 0, size);
    memcpy(arena + base, file, headers);
    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t *s = file + sections + i * 40u;
        uint32_t vsz = le32(s + 8), va = le32(s + 12), rsz = le32(s + 16), raw = le32(s + 20);
        uint32_t copy = !vsz || rsz < vsz ? rsz : vsz;
        if (copy) memcpy(arena + base + va, file + raw, copy);
    }
    *layout = (Sh3PeLayout){base, size, base + entry_rva, (uint32_t)heap};
    return 0;
}
