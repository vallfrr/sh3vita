#include "vita_paths.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    char out[256];
    assert(!sh3_vita_data_path("C:\\SILENT HILL 3\\data/sound/sd.afs",out,sizeof out));
    assert(!strcmp(out,"ux0:data/sh3/data/sound/sd.afs"));
    assert(!sh3_vita_data_path("c:\\silent hill 3\\DATA\\pic\\konami.bmp",out,sizeof out));
    assert(!strcmp(out,"ux0:data/sh3/data/pic/konami.bmp"));
    assert(sh3_vita_data_path("C:\\SILENT HILL 3\\data/../sh4/sh4.exe",out,sizeof out));
    assert(sh3_vita_data_path("C:\\SILENT HILL 3\\data2/file",out,sizeof out));
    assert(sh3_vita_data_path("C:\\SILENT HILL 3\\data/a:evil",out,sizeof out));
    assert(sh3_vita_data_path("C:\\SILENT HILL 3\\data//file",out,sizeof out));
    assert(sh3_vita_data_path("C:\\SILENT HILL 3\\data/file",out,8));
    puts("Vita SH3 path boundary checks passed");
}
