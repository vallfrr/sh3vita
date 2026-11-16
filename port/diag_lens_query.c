/* Flashlight lens-flare visibility test (F_0049fac0, only caller F_00468180 -> F_00469e70).
 *
 * The PC game decides whether Heather's flashlight halo is visible with an occlusion probe: per frame it switches the
 * render target to a 1x1 surface, samples the scene pixel at the lamp, draws a white marker point at the lamp's depth,
 * samples again, LockRect-reads both samples, then repaints the marker pixel. F_00468180 needs SIX consecutive positive
 * answers before the halo intensity (0x898428) starts to rise; F_004690a0 uses that intensity as the colour of the
 * ~24 halo ray sprites and only draws them when it is >= 0.001.
 *
 * On the Vita this path is both slow and wrong:
 *  - four mid-frame render-target switches and two readbacks per frame on a tile-based GPU (7-12 ms measured in the
 *    park, docs/HANDOFF-RUNTIME.md "lensasync"): the drop to the 3-tick / 20 Hz plateau whenever the lamp faces the camera;
 *  - draws into the 1x1 target have no effect (GXM), and with the deferred readback (lensasync) the decisive second lock
 *    returns the value queued by the FIRST lock (the scene colour, not the marker). The answer then follows the scene
 *    pixel's blue byte, flickers on dark pixels, never holds six frames: halo intensity stays ~0 (sprites drawn black)
 *    and pumps up and down (the brightness changes Valentin reported). The game also repaints the lamp pixel with a
 *    stale colour.
 *
 * The engine has its own switch for this: when the setting "lensflare_ztest" reads 1, F_0049fac0 returns "visible"
 * before any render-target switch, draw or readback (PC low lens-flare quality). The setting is read nowhere else.
 * It is forced on the Vita by default. Cost: the flashlight halo is no longer hidden when the lamp itself is behind
 * an obstacle (angle/distance gating in F_00468840 is unchanged). Marker ux0:data/sh3/diag/lensprobe (env
 * SH3_DIAG_LENSPROBE) restores the original probe for A/B. Other platforms keep the original getter unless
 * SH3_DIAG_LENSQUERY_OFF is set. NOT yet verified on hardware (2026-11-16, written from code analysis). */
#include "sh4base/rt/rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void __real_F_005ef950(CPU *c);

void __wrap_F_005ef950(CPU *c) {
    HOSTFN;
    static int enabled = -1;
    static unsigned forced;
    if (enabled < 0) {
#ifdef __vita__
        enabled = getenv("SH3_DIAG_LENSPROBE") == NULL;
#else
        enabled = getenv("SH3_DIAG_LENSQUERY_OFF") != NULL;
#endif
    }
    uint32_t key = enabled ? R32(c->esp + 4) : 0;
    int matched = enabled && key <= g_mem_size &&
                  sizeof("lensflare_ztest") <= g_mem_size - key &&
                  !memcmp(g_mem + key, "lensflare_ztest", sizeof("lensflare_ztest"));
    __real_F_005ef950(c);
    if (matched) {
        unsigned original = c->eax;
        c->eax = 1; /* F_0049fac0 returns "visible" before its render-target switches and readbacks. */
        if (++forced <= 3) {
            fprintf(stderr, "[lens-query] lensflare_ztest original=%u forced=1 call=%u (probe bypassed)\n", original, forced);
            fflush(stderr);
        }
    }
}
