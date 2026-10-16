#define main hades_app_main
#include "../ios/main.m"
#undef main
#include <assert.h>
#include <objc/runtime.h>

/* Host services are test doubles; only the actual UIKit controller and resource
   verifier run here. This probe never executes guest code or accesses saves. */
const char hades_render_resolution[]="720p";
static uint64_t submitted;
int hades_run_core_probe(int argc,const char *const *argv) { (void)argc;(void)argv;abort(); }
int hades_run_render_probe(int argc,const char *const *argv) { (void)argc;(void)argv;abort(); }
int hades_vulkan_configure_surface(void *layer) { (void)layer;return 0; }
int hades_vulkan_configure_capture(const char *path,unsigned step) { (void)path;(void)step;abort(); }
uint64_t hades_vulkan_present_count(void) { return submitted; }
int hades_vulkan_drain_lifecycle(void) { abort(); }
int hades_ipc_lifecycle_transition(int suspended) { (void)suspended;abort(); }
void hades_ipc_wake_lifecycle(void) {}
void hades_threads_wake_lifecycle(void) {}
int hades_ipc_set_focus_state(unsigned state) { (void)state;return 0; }
int hades_ipc_configure_input_frontend(void) { return 0; }
int hades_ipc_publish_input(uint64_t buttons,int32_t x,int32_t y) {
    (void)buttons;(void)x;(void)y;return 0;
}

@interface HadesStartupController : HadesViewController
@property(nonatomic) unsigned launches;
@property(nonatomic) unsigned lastMode;
@end
@implementation HadesStartupController
- (void)runMode:(unsigned)mode { self.launches++;self.lastMode=mode; }
@end

static void resources(void) {
    NSString *directory=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSFileManager *files=NSFileManager.defaultManager;
    NSError *error=nil;
    assert([files createDirectoryAtPath:directory withIntermediateDirectories:NO attributes:nil error:&error]);
    NSData *data=[@"synthetic resource" dataUsingEncoding:NSUTF8StringEncoding];
    NSString *path=[directory stringByAppendingPathComponent:@"fixture"];
    assert([data writeToFile:path options:NSDataWritingAtomic error:&error]);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes,(CC_LONG)data.length,digest);
    NSMutableString *hex=[NSMutableString new];
    for (unsigned i=0;i<sizeof digest;i++) [hex appendFormat:@"%02x",digest[i]];
    NSDictionary *records=@{@"RomFS/fixture":@{@"bytes":@(data.length),@"sha256":hex}};
    NSString *catalog=[directory stringByAppendingPathComponent:@"catalog.json"];
    assert([[NSJSONSerialization dataWithJSONObject:records options:0 error:&error]
        writeToFile:catalog options:NSDataWritingAtomic error:&error]);
    __block NSUInteger calls=0;
    assert(verify_romfs(directory,catalog,^(NSUInteger verified,NSUInteger total) {
        assert(total==1 && verified==calls);
        calls++;
    }));
    assert(calls==2);
    NSMutableData *corrupt=[data mutableCopy];
    ((unsigned char *)corrupt.mutableBytes)[0]^=1;
    assert([corrupt writeToFile:path options:NSDataWritingAtomic error:&error]);
    assert(!verify_romfs(directory,catalog,nil));
    assert([files removeItemAtPath:directory error:&error]);
}

static UIApplicationState appState=UIApplicationStateActive;
static UIApplicationState observedState(id object,SEL selector) {
    (void)object;(void)selector;return appState;
}

static void exercise(UIWindow *window,HadesStartupController *controller) {
    Method state=class_getInstanceMethod(UIApplication.class,@selector(applicationState));
    IMP original=method_setImplementation(state,(IMP)observedState);
    requestedProbe=3;
    appState=UIApplicationStateInactive;
    [controller runRequestedProbe];
    assert(!controller.launches && requestedProbe==3);
    appState=UIApplicationStateActive;
    [controller runRequestedProbe];
    [controller runRequestedProbe];
    assert(controller.launches==1 && controller.lastMode==3 && requestedProbe==-1);
    for (int explicit=-2;explicit<=1;explicit++) {
        requestedProbe=explicit;
        unsigned before=controller.launches;
        [controller runRequestedProbe];
        assert(controller.launches==before+(explicit>=0));
        if (explicit>=0) assert(controller.lastMode==(unsigned)explicit);
    }
    requestedProbe=-1;
    [controller installGameLayout];
    controller.gameContainer.frame=CGRectMake(0,0,932,430);
    [controller viewDidLayoutSubviews];
    assert(controller.diagnosticButton.superview==controller.systemMonitor);
    assert(controller.diagnosticButton.superview!=controller.gameContainer);
    assert(controller.systemMonitor.hidden);
    assert(CGRectContainsRect(controller.systemMonitor.bounds,controller.diagnosticButton.frame));
    assert(!CGRectIntersectsRect(controller.monitorText.frame,controller.diagnosticButton.frame));
    assert(!controller.loadingView.hidden && !controller.loadingView.userInteractionEnabled);
    [controller toggleSystemMonitor];
    assert(!controller.systemMonitor.hidden);
    assert([controller.diagnosticButton hitTest:CGPointMake(
        CGRectGetMidX(controller.diagnosticButton.bounds),
        CGRectGetMidY(controller.diagnosticButton.bounds)) withEvent:nil]==controller.diagnosticButton);
    [controller.diagnosticButton sendActionsForControlEvents:UIControlEventTouchUpInside];
    assert(controller.gameContainer.hidden && !controller.diagnosticStack.hidden);
    assert(controller.systemMonitor.hidden);
    [controller showGameSurface];
    assert(!controller.gameContainer.hidden && controller.diagnosticStack.hidden);
    for (UIButton *button in controller.inputButtons) button.enabled=NO;
    controller.joystick.enabled=NO;
    [controller updateFrameRate];
    assert(!controller.loadingView.hidden && !controller.joystick.enabled);
    submitted=1;
    [controller updateFrameRate];
    assert(controller.loadingView.hidden && controller.joystick.enabled);
    for (UIButton *button in controller.inputButtons) assert(button.enabled);
    assert(window.rootViewController==controller);
    method_setImplementation(state,original);
    resources();
    puts("UIKIT_AUTOSTART_MONITOR_LOG_AND_LOADING_OK");
    exit(0);
}

@interface HadesStartupProbeScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation HadesStartupProbeScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)options {
    (void)session;(void)options;
    requestedProbe=3;
    HadesStartupController *controller=[HadesStartupController new];
    self.window=[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController=controller;
    [controller loadViewIfNeeded];
    assert(controller.diagnosticStack.hidden);
    [self.window makeKeyAndVisible];
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    HadesStartupController *controller=(HadesStartupController *)self.window.rootViewController;
    controller.launches=0;
    requestedProbe=3;
    Method state=class_getInstanceMethod(UIApplication.class,@selector(applicationState));
    IMP original=method_setImplementation(state,(IMP)observedState);
    appState=UIApplicationStateInactive;
    HadesSceneDelegate *delegate=[HadesSceneDelegate new];
    delegate.window=self.window;
    [delegate sceneDidBecomeActive:scene];
    assert(!controller.launches && requestedProbe==3);
    method_setImplementation(state,original);
    dispatch_async(dispatch_get_main_queue(), ^{
        assert(controller.launches==1 && controller.lastMode==3 && requestedProbe==-1);
        controller.launches=0;
        exercise(self.window,controller);
    });
}
@end
@interface HadesStartupProbeApp : UIResponder <UIApplicationDelegate>
@end
@implementation HadesStartupProbeApp
@end
int main(int argc,char **argv) {
    @autoreleasepool {
        logPath=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        return UIApplicationMain(argc,argv,nil,NSStringFromClass(HadesStartupProbeApp.class));
    }
}
