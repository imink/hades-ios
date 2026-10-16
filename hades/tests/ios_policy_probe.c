#include "../ios/policy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    int mode;
    char *none[]={"probe"};
    char *core[]={"probe","--hades-probe","core"};
    char *gpu[]={"probe","--hades-probe","gpu"};
    char *game[]={"probe","--hades-probe","game"};
    char *sdk[]={"probe","--hades-probe","sdk"};
    char *missing[]={"probe","--hades-probe"};
    char *twice[]={"probe","--hades-probe","core","--hades-probe","gpu"};
    char *diagnostics[]={"probe","--hades-diagnostics"};
    char *both[]={"probe","--hades-diagnostics","--hades-probe","core"};
    char *reverse[]={"probe","--hades-probe","gpu","--hades-diagnostics"};
    char *duplicate[]={"probe","--hades-diagnostics","--hades-diagnostics"};
    assert(!hades_ios_requested_probe(1,none,&mode) && mode==-1);
    assert(!hades_ios_requested_probe(3,core,&mode) && mode==0);
    assert(!hades_ios_requested_probe(3,gpu,&mode) && mode==1);
    assert(hades_ios_requested_probe(3,game,&mode)==-1);
    assert(hades_ios_requested_probe(3,sdk,&mode)==-1);
    assert(hades_ios_requested_probe(2,missing,&mode)==-1);
    assert(hades_ios_requested_probe(5,twice,&mode)==-1);
    assert(!hades_ios_requested_probe(2,diagnostics,&mode) && mode==-2);
    assert(hades_ios_requested_probe(4,both,&mode)==-1);
    assert(hades_ios_requested_probe(4,reverse,&mode)==-1);
    assert(hades_ios_requested_probe(3,duplicate,&mode)==-1);
    assert(hades_ios_initial_mode(1,-1)==3);
    assert(hades_ios_initial_mode(0,-1)==-1);
    for (int requested=-2;requested<=1;requested++)
        if (requested!=-1)
            assert(hades_ios_initial_mode(1,requested)==requested);
    for (unsigned mode=0;mode<4;mode++) {
        assert(!hades_ios_background_requires_exit(0,mode));
        assert(hades_ios_background_requires_exit(1,mode)==(mode>=1));
    }
    assert(hades_ios_background_requires_exit(1,99));
    puts("IOS_FOREGROUND_DIAGNOSTIC_POLICY_OK");
    return 0;
}
