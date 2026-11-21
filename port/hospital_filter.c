#include "sh4base/rt/rt.h"

extern void __real_F_0041e200(CPU *c);

void __wrap_F_0041e200(CPU *c) {
    const uint8_t *const mb = g_mem;
    const uint32_t stage = R8(0x70e66d9u);
    const uint32_t room = R32(0x897e48u);
    const uint32_t taps_argument = c->esp + 8u;

    /* F_00420370 passes two taps per axis for these hospital scenes.
       One tap retains the measured gain in 31:197. In 32:208 the
       extra filter passes account for almost half the frame time. */
    if (((stage == 31 && room == 197) || (stage == 32 && room == 208)) &&
        R32(0xbcc660u) == 5 && !R8(0xbcc218u) &&
        R32(c->esp) == 0x420748u && R32(taps_argument) == 2) {
        W32(taps_argument, stage == 31 ? 1 : 0);
        __real_F_0041e200(c);
        W32(taps_argument, 2);
        return;
    }
    __real_F_0041e200(c);
}
