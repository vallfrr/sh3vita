#include <assert.h>
#include <stdlib.h>
#include <string.h>
void w32_host_path(const char *,char *,size_t);
int main(void) {
 char out[128]; setenv("SH3_DIAG_MOVIE_PC","1",1);
 w32_host_path("C:\\SILENT HILL 3\\data\\movie\\op.000",out,sizeof out);
 assert(!strcmp(out,"build/pc-movie-lab/op.mpg"));
 w32_host_path("data/movie/ED_U.000",out,sizeof out);
 assert(!strcmp(out,"build/pc-movie-lab/ed_u.mpg"));
 w32_host_path("data/movie/../../op.000",out,sizeof out);assert(!out[0]);
 w32_host_path("data/movie/other.000",out,sizeof out);assert(!out[0]);
 w32_host_path("op.000",out,5);assert(!out[0]);
 unsetenv("SH3_DIAG_MOVIE_PC");w32_host_path("op.000",out,sizeof out);assert(!out[0]);
 setenv("SH3_DIAG_MOVIE_PC_SHORT","1",1);
 w32_host_path("data/movie/op.000",out,sizeof out);assert(!strcmp(out,"build/pc-movie-lab/converted/sh3-pc-op-10s.mpg"));
 w32_host_path("data/movie/ed_n.000",out,sizeof out);assert(!out[0]);
 w32_host_path("../op.000",out,sizeof out);assert(!out[0]);
 w32_host_path("op.000",out,5);assert(!out[0]);
 return 0;
}
