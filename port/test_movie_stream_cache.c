/* Differential cursor/cache test. Run against an owned, nonempty movie file.
 * Compile with SH3_MOVIE_LAB=1 to exercise both stdio and direct reads. */
#define SDL_MAIN_HANDLED
#include "sh4base/dx/movie_stream_cache.h"
#include <assert.h>

typedef struct { MovieCursor cursor; const char *path; unsigned seed; } Reader;
static int check_cursor(void *arg) {
    Reader *r = arg;
    FILE *reference = fopen(r->path, "rb");
    assert(reference);
    unsigned char actual[8192], expected[8192];
    for (unsigned i = 0; i < 2000; i++) {
        r->seed = r->seed * 1664525u + 1013904223u;
        size_t pos = r->seed % (r->cursor.source->size + 1);
        r->seed = r->seed * 1664525u + 1013904223u;
        size_t count = r->seed % sizeof actual;
        if (i < 12) pos = i * (MOVIE_STREAM_BLOCK / 2) - (i > 0);
        if (pos > r->cursor.source->size) pos = r->cursor.source->size;
        r->cursor.pos = pos;
        assert(!fseek(reference, (long)pos, SEEK_SET));
        size_t want = fread(expected, 1, count, reference);
        size_t got = movie_stream_read(&r->cursor, actual, count);
        assert(!ferror(reference) && got == want && !memcmp(actual, expected, got));
        assert(r->cursor.pos == pos + got);
    }
    r->cursor.pos = r->cursor.source->size;
    assert(movie_stream_read(&r->cursor, actual, sizeof actual) == 0);
    fclose(reference);
    return 0;
}
int main(int argc, char **argv) {
    assert(argc == 2 && SDL_Init(0) == 0);
    for (int direct = 0; direct <= 1; direct++) {
        if (direct) SDL_setenv("SH3_DIAG_MOVIE_DIRECT_IO", "1", 1);
        else SDL_setenv("SH3_DIAG_MOVIE_DIRECT_IO", "", 1);
        MovieStream *s = movie_stream_open(argv[1]);
        assert(s);
#ifdef SH3_MOVIE_LAB
        /* An empty environment value also means present; clear explicitly. */
        s->direct_io = direct;
#endif
        Reader a = {{s, 0}, argv[1], 0x12345678u};
        Reader b = {{s, 0}, argv[1], 0x9abcdef0u};
        SDL_Thread *ta = SDL_CreateThread(check_cursor, "video-test", &a);
        SDL_Thread *tb = SDL_CreateThread(check_cursor, "audio-test", &b);
        assert(ta && tb);
        SDL_WaitThread(ta, NULL); SDL_WaitThread(tb, NULL);
        printf("movie cache mode=%s: 4000 concurrent cursor comparisons PASS\n", direct ? "direct" : "stdio");
        movie_stream_close(s);
    }
    SDL_Quit();
    return 0;
}
