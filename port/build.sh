#!/bin/bash
# Vita build from Linux or WSL:   bash port/build.sh
# Needs: VitaSDK (VITASDK), CMake, Ninja, Python 3, port/gen/ (python recomp/x86rec.py) and a patched vitaGL
# (see docs/WORKFLOW.md). SH3_VITAGL_DIR points at the vitaGL checkout that holds libvitaGL.a (default ~/src/vitaGL).
# Result: build/sh3vita.vpk
set -euo pipefail
export VITASDK="${VITASDK:-$HOME/vitasdk}"   # set VITASDK if your VitaSDK lives elsewhere
export PATH="$VITASDK/bin:$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin:$PATH"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
# Build outside the source tree: compiling on a Windows drive from WSL is slow.
B="${B:-$HOME/build/sh3vita}"
export SH3_VITAGL_DIR="${SH3_VITAGL_DIR:-$HOME/src/vitaGL}"
vgl_lib="$SH3_VITAGL_DIR/libvitaGL.a"
test -f "$vgl_lib" || { echo "vitaGL not found: $vgl_lib (build the patched vitaGL first, see docs/WORKFLOW.md)" >&2; exit 1; }
test -d port/gen && ls port/gen/*.c > /dev/null 2>&1 || { echo "port/gen is empty: run python recomp/x86rec.py first" >&2; exit 1; }

# A GXM scene ends whenever the game switches render targets (lighting, shadows, blur, menus). Losing depth/stencil
# there makes later draws disappear: refuse a vitaGL built without STORE_DEPTH_STENCIL.
for mode in Load Store; do
    if ! "$VITASDK/bin/arm-vita-eabi-nm" -u "$vgl_lib" | grep -F "sceGxmDepthStencilSurfaceSetForce${mode}Mode" > /dev/null; then
        echo "vitaGL must preserve depth/stencil between scenes: rebuild it with  make -B NO_DEBUG=1 STORE_DEPTH_STENCIL=1" >&2
        exit 1
    fi
done

on=(ARCHIVE_STREAM CONSOLE_CONTROLS CONSOLE_MESSAGES CONSOLE_REFERENCE
    CONTROLS_ARROWS_LAB CRT_COPY_JUMPS INLINE_SQRT X87_INLINE MATRIX_LAB
    FRAME_POLICY_LAB PARTICLE_NATIVE_LAB SHADOW_NATIVE_LAB FXHALF_RES_LAB
    PLAYTEST_PROFILE PLAYTEST_PERFDEFAULT_LAB RAND_LAB MOVIE_LAB MOVIE_O3_LAB MOVIE_HW_LAB MOVIE_PLAYTEST_DEFAULT)
off=(PROFILE PROFILE_WORKER_PARTS NUMERIC_LAB NATIVE_CLIP_LAB
     GEOMETRY_INLINE_LAB PC24_VEC3_LAB FLOOR_INLINE_LAB RENDERSTATE_LAB
     LEAF_RESTRICT_LAB CPU_RESTRICT_LAB SAMPLER_CACHE_LAB D3D_FPU_CONTRACT_LAB HOT_OPT)
args=()
for option in "${on[@]}"; do args+=("-DSH3_$option=ON"); done
for option in "${off[@]}"; do args+=("-DSH3_$option=OFF"); done

cmake -S port -B "$B" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DSH3_GUEST_MEMORY_MB=256 -DSH3_OPT_LEVEL=s \
    -DSH3_EXTRA_CFLAGS= -DSH3_EXTRA_LDFLAGS= -DSH3_WORKER_UNIT= \
    -DSH3_VITAGL_DIR="$SH3_VITAGL_DIR" -DSH3_VITAGL_LIB="$vgl_lib" "${args[@]}" "$@"
cmake --build "$B" -j"$(nproc)"
mkdir -p build && cp "$B/sh3vita.vpk" build/
sha256sum build/sh3vita.vpk
