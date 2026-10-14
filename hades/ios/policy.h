#pragma once
#include <string.h>

static inline int hades_ios_requested_probe(int argc,char **argv,int *mode) {
    *mode=-1;
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"--hades-probe"))
            continue;
        if (*mode!=-1 || ++i>=argc)
            return -1;
        if (!strcmp(argv[i],"core"))
            *mode=0;
        else if (!strcmp(argv[i],"gpu"))
            *mode=1;
        else
            return -1;
    }
    return 0;
}
static inline int hades_ios_background_requires_exit(int running,unsigned mode) {
    return running && mode>=1;
}
