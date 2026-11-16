/* Verify the target's emulated TLS before any game/render threads start. */
#include <pthread.h>
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>

static __thread unsigned tls_probe;
struct TlsProbe { uintptr_t address; unsigned initial; };

static void *tls_probe_worker(void *opaque) {
    struct TlsProbe *p = opaque;
    p->address = (uintptr_t)&tls_probe;
    p->initial = tls_probe;
    tls_probe = 0x2468aceu;
    return NULL;
}

static int tls_probe_sdl_worker(void *opaque) {
    tls_probe_worker(opaque);
    return 0;
}

int diag_tls_verify(void) {
    struct TlsProbe probe = {0};
    pthread_t worker;
    tls_probe = 0x13579bdu;
    int created = pthread_create(&worker, NULL, tls_probe_worker, &probe);
    int joined = created ? -1 : pthread_join(worker, NULL);
    int isolated = !created && !joined && probe.address != (uintptr_t)&tls_probe &&
                   probe.initial == 0 && tls_probe == 0x13579bdu;
    int posix_ok = isolated;
    probe = (struct TlsProbe){0};
    SDL_Thread *sdl_worker = posix_ok ? SDL_CreateThread(tls_probe_sdl_worker, "sh3-tls-check", &probe) : NULL;
    int sdl_status = -1;
    if (sdl_worker) SDL_WaitThread(sdl_worker, &sdl_status);
    int sdl_ok = sdl_worker && sdl_status == 0 && probe.initial == 0 &&
                 probe.address != (uintptr_t)&tls_probe && tls_probe == 0x13579bdu;
    isolated = posix_ok && sdl_ok;
    fprintf(stderr, "[tls] isolation=%s pthread=%s sdl=%s create=%d join=%d\n",
            isolated ? "PASS" : "FAIL", posix_ok ? "PASS" : "FAIL",
            sdl_ok ? "PASS" : "FAIL", created, joined);
    fflush(stderr);
    tls_probe = 0;
    return isolated ? 0 : -1;
}
