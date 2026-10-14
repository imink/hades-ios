#import <UIKit/UIKit.h>
#include "../runtime/probes.h"
#include "../runtime/ipc.h"
#include "../runtime/diagnostics.h"
#include "policy.h"
#include <CommonCrypto/CommonDigest.h>
#include <os/proc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>
#ifdef HADES_VULKAN_HOST
#import <QuartzCore/CAMetalLayer.h>
#include "../runtime/vulkan.h"
@interface HadesSurfaceView : UIView
@end
@implementation HadesSurfaceView
+ (Class)layerClass { return CAMetalLayer.class; }
@end
#endif

static NSString *logPath;
static int requestedProbe=-1;

@interface HadesViewController : UIViewController
@property(nonatomic, strong) UITextView *logView;
@property(nonatomic, strong) NSArray<UIButton *> *runButtons;
@property(nonatomic) BOOL running;
@property(nonatomic) unsigned activeMode;
@property(nonatomic, strong) NSArray<UIButton *> *inputButtons;
@property(nonatomic, strong) UIView *gameContainer;
@property(nonatomic, strong) NSLayoutConstraint *surfaceHeight;
@property(nonatomic, strong) UIStackView *diagnosticStack;
@property(nonatomic, strong) UIButton *diagnosticButton;
@property(nonatomic) BOOL inputConfigured;
#ifdef HADES_VULKAN_HOST
@property(nonatomic, strong) HadesSurfaceView *surface;
#endif
- (void)executeMode:(unsigned)mode;
@end

typedef struct { unsigned mode; void *controller; } HadesWork;
static void *probe_thread(void *argument) {
    HadesWork *work = argument;
    unsigned mode = work->mode;
    @autoreleasepool {
        HadesViewController *controller = (__bridge_transfer HadesViewController *)work->controller;
        free(work);
        [controller executeMode:mode];
        if (mode >= 2)
            for (;;)
                pause();
    }
    return NULL;
}

static BOOL verify_romfs(NSString *directory, NSString *catalog) {
    NSError *error = nil;
    NSDictionary *root = [NSFileManager.defaultManager attributesOfItemAtPath:directory error:&error];
    if (![root[NSFileType] isEqualToString:NSFileTypeDirectory]) {
        fprintf(stderr, "[Hades-iOS] Documents/RomFS is missing or not a real directory\n");
        return NO;
    }
    NSData *data = [NSData dataWithContentsOfFile:catalog options:0 error:&error];
    id decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
    if (![decoded isKindOfClass:NSDictionary.class] || ![decoded count]) {
        fprintf(stderr, "[Hades-iOS] Invalid RomFS inventory: %s\n", error.localizedDescription.UTF8String ?: "invalid shape");
        return NO;
    }
    NSDictionary *records = decoded;
    for (NSString *name in records) {
        if (![name isKindOfClass:NSString.class] || ![name hasPrefix:@"RomFS/"])
            return NO;
        NSString *relative = [name substringFromIndex:6];
        NSString *path = directory;
        for (NSString *part in relative.pathComponents) {
            if (![part length] || [part isEqualToString:@"."] || [part isEqualToString:@".."]
                || [part isEqualToString:@"/"] || [part containsString:@"\\"])
                return NO;
            NSArray *entries = [NSFileManager.defaultManager contentsOfDirectoryAtPath:path error:&error];
            if (![entries containsObject:part]) {
                fprintf(stderr, "[Hades-iOS] Missing/exact-case resource component: %s\n", part.UTF8String);
                return NO;
            }
            path = [path stringByAppendingPathComponent:part];
            NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:&error];
            if (!attributes || [attributes[NSFileType] isEqualToString:NSFileTypeSymbolicLink])
                return NO;
        }
        NSDictionary *record = records[name];
        NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:&error];
        if (![record isKindOfClass:NSDictionary.class]
            || ![attributes[NSFileType] isEqualToString:NSFileTypeRegular]
            || [attributes[NSFileSize] unsignedLongLongValue] != [record[@"bytes"] unsignedLongLongValue]) {
            fprintf(stderr, "[Hades-iOS] Invalid resource type/size: %s\n", relative.UTF8String);
            return NO;
        }
        FILE *file = fopen(path.fileSystemRepresentation, "rb");
        if (!file)
            return NO;
        CC_SHA256_CTX context;
        CC_SHA256_Init(&context);
        unsigned char buffer[65536], digest[CC_SHA256_DIGEST_LENGTH];
        size_t count;
        while ((count = fread(buffer, 1, sizeof buffer, file)))
            CC_SHA256_Update(&context, buffer, (CC_LONG)count);
        int failed = ferror(file);
        if (fclose(file))
            failed = 1;
        CC_SHA256_Final(digest, &context);
        NSMutableString *hex = [NSMutableString string];
        for (unsigned i = 0; i < sizeof digest; i++)
            [hex appendFormat:@"%02x", digest[i]];
        if (failed || ![hex isEqualToString:record[@"sha256"]]) {
            fprintf(stderr, "[Hades-iOS] Resource hash/read failure: %s\n", relative.UTF8String);
            return NO;
        }
    }
    fprintf(stderr, "[Hades-iOS] ROMFS_VERIFIED files=%lu; private USB resources only\n", (unsigned long)records.count);
    return YES;
}

@implementation HadesViewController
- (void)runRequestedProbe {
    if (requestedProbe<0 || self.running || !self.view.window
        || UIApplication.sharedApplication.applicationState!=UIApplicationStateActive)
        return;
    unsigned mode=(unsigned)requestedProbe;
    requestedProbe=-1;
    fprintf(stderr,"[Hades-iOS] EXPLICIT_PROBE mode=%u; core/GPU only, no SDK/game autostart\n",mode);
    [self runMode:mode];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self runRequestedProbe];
}
- (void)didReceiveMemoryWarning {
    [super didReceiveMemoryWarning];
    fprintf(stderr,"[Hades-iOS] MEMORY_WARNING mode=%u running=%d available=%llu; no unverified heap reduction or save claim\n",
            self.activeMode,self.running,(unsigned long long)os_proc_available_memory());
    hades_log_memory("UIKit-memory-warning",0,0,0);
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return self.gameContainer ? UIInterfaceOrientationMaskLandscape : UIInterfaceOrientationMaskAllButUpsideDown;
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
#ifdef HADES_VULKAN_HOST
    if (self.gameContainer) {
        CGRect bounds=self.gameContainer.bounds;
        CGFloat width=MIN(bounds.size.width,bounds.size.height*16.0/9.0);
        self.surface.frame=CGRectMake((bounds.size.width-width)/2,(bounds.size.height-width*9.0/16.0)/2,width,width*9.0/16.0);
        CAMetalLayer *layer=(CAMetalLayer *)self.surface.layer;
        if (width>0) {
            layer.contentsScale=1280.0/width;
            layer.drawableSize=CGSizeMake(1280,720);
        }
        const CGFloat positions[12][2]={{.13,.68},{.13,.9},{.065,.79},{.195,.79},
            {.86,.78},{.92,.89},{.92,.67},{.98,.78},{.08,.1},{.92,.1},{.28,.79},{.5,.08}};
        UIEdgeInsets inset=self.view.safeAreaInsets;
        CGFloat available=bounds.size.width-inset.left-inset.right;
        for (unsigned i=0;i<self.inputButtons.count;i++) {
            CGFloat x=inset.left+available*positions[i][0];
            x=MAX(inset.left+28,MIN(bounds.size.width-inset.right-28,x));
            CGFloat y=MAX(inset.top+28,MIN(bounds.size.height-inset.bottom-28,bounds.size.height*positions[i][1]));
            self.inputButtons[i].frame=CGRectMake(x-26,y-26,52,52);
        }
        self.diagnosticButton.frame=CGRectMake(bounds.size.width/2-32,
            bounds.size.height-inset.bottom-40,64,32);
    }
#endif
}
- (void)installGameLayout {
#ifdef HADES_VULKAN_HOST
    self.gameContainer=[[UIView alloc] initWithFrame:self.view.bounds];
    self.gameContainer.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    self.gameContainer.backgroundColor=UIColor.blackColor;
    self.surfaceHeight.active=NO;
    UIStackView *stack=(UIStackView *)self.surface.superview;
    [stack removeArrangedSubview:self.surface];
    [self.surface removeFromSuperview];
    stack.hidden=YES;
    [self.view addSubview:self.gameContainer];
    self.surface.translatesAutoresizingMaskIntoConstraints=YES;
    [self.gameContainer addSubview:self.surface];
    for (UIButton *button in self.inputButtons) {
        [(UIStackView *)button.superview removeArrangedSubview:button];
        [button removeFromSuperview];
        button.translatesAutoresizingMaskIntoConstraints=YES;
        button.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:.5];
        button.layer.cornerRadius=26;
        button.tintColor=UIColor.whiteColor;
        [self.gameContainer addSubview:button];
    }
    self.diagnosticButton=[UIButton buttonWithType:UIButtonTypeSystem];
    [self.diagnosticButton setTitle:@"日志" forState:UIControlStateNormal];
    self.diagnosticButton.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:.5];
    self.diagnosticButton.tintColor=UIColor.whiteColor;
    [self.diagnosticButton addTarget:self action:@selector(showDiagnostics) forControlEvents:UIControlEventTouchUpInside];
    [self.gameContainer addSubview:self.diagnosticButton];
    [self setNeedsUpdateOfSupportedInterfaceOrientations];
    UIWindowSceneGeometryPreferencesIOS *geometry=[[UIWindowSceneGeometryPreferencesIOS alloc]
        initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
    [self.view.window.windowScene requestGeometryUpdateWithPreferences:geometry errorHandler:^(NSError *error) {
        fprintf(stderr,"[Hades-iOS] Landscape transition unavailable: %s\n",error.localizedDescription.UTF8String);
    }];
    [self.view setNeedsLayout];
    [self.view layoutIfNeeded];
#endif
}
- (void)publishControls {
    uint64_t buttons=0;
    for (UIButton *button in self.inputButtons)
        if (button.selected) buttons|=1ull<<button.tag;
    int32_t x=(buttons&(1u<<14) ? 32767 : 0)-(buttons&(1u<<12) ? 32767 : 0);
    int32_t y=(buttons&(1u<<13) ? 32767 : 0)-(buttons&(1u<<15) ? 32767 : 0);
    if (hades_ipc_publish_input(buttons&~0xf000ull,x,y))
        fprintf(stderr,"[Hades-iOS] Cannot publish touch-controller state\n");
}
- (void)controlDown:(UIButton *)button { button.selected=YES;[self publishControls]; }
- (void)controlUp:(UIButton *)button { button.selected=NO;[self publishControls]; }
- (void)releaseControls {
    for (UIButton *button in self.inputButtons)
        button.selected=NO;
    if (self.inputConfigured)
        [self publishControls];
}
- (void)showDiagnostics {
    [self releaseControls];
    self.gameContainer.hidden=YES;
    self.diagnosticStack.hidden=NO;
    [self refreshLog];
}
- (void)showGameSurface {
    if (self.gameContainer) {
        self.diagnosticStack.hidden=YES;
        self.gameContainer.hidden=NO;
        [self.view setNeedsLayout];
    }
}
- (UIButton *)button:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:title forState:UIControlStateNormal];
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    UILabel *title = [UILabel new];
    title.text = @"Hades — 真机移植验证";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    UILabel *notice = [UILabel new];
    notice.numberOfLines = 0;
    notice.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    notice.text = @"面向 iOS 26 / iPhone 15 Pro Max，仍非已验证可玩版。Mac 原生 GPU 读回与受控按键已验证菜单；完整玩法与真机表现尚未验证。\n需 USB 放入完整 RomFS。采用 2 GiB 诊断堆；GPU/SDK/游戏诊断切后台会终止，未提交进度可能丢失。客体线程暂停及通用库小程序仍未实现。";
    UIButton *core = [self button:@"1. 验证真实原生映像 / EOS / 计时门" action:@selector(runCore)];
    UIButton *gpu = [self button:@"2. 验证 72 份 MSL 与离屏 GPU 读回" action:@selector(runGPU)];
    UIButton *sdk = [self button:@"3. 执行真实 SDK 启动（可能退出）" action:@selector(runSDK)];
    UIButton *game = [self button:@"4. 执行真实游戏入口（尚非试玩）" action:@selector(runGame)];
    self.runButtons = @[core, gpu, sdk, game];
    UIButton *refresh = [self button:@"刷新本机日志" action:@selector(refreshLog)];
    UIButton *export = [self button:@"导出诊断日志（请勿公开发布）" action:@selector(exportLog:)];
    UIButton *resume = [self button:@"返回游戏画面（不会暂停客体）" action:@selector(showGameSurface)];
    self.logView = [UITextView new];
    self.logView.editable = NO;
    self.logView.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    self.logView.accessibilityIdentifier = @"hades-diagnostic-log";
    NSMutableArray *views=[NSMutableArray arrayWithArray:@[title, notice, core, gpu, sdk, game]];
#ifdef HADES_VULKAN_HOST
    self.surface=[HadesSurfaceView new];
    self.surface.backgroundColor=UIColor.blackColor;
    self.surface.accessibilityIdentifier=@"hades-real-metal-surface";
    self.surfaceHeight=[self.surface.heightAnchor constraintEqualToConstant:120];
    self.surfaceHeight.active=YES;
    [views addObject:self.surface];
    NSArray<NSString *> *labels=@[@"上",@"下",@"左",@"右",@"A",@"B",@"X",@"Y",@"L",@"R",@"ZL",@"+"];
    const unsigned bits[]={13,15,12,14,0,1,2,3,6,7,8,10};
    NSMutableArray<UIButton *> *controls=[NSMutableArray new];
    for (unsigned row=0;row<3;row++) {
        NSMutableArray *items=[NSMutableArray new];
        for (unsigned column=0;column<4;column++) {
            unsigned index=row*4+column;
            UIButton *control=[UIButton buttonWithType:UIButtonTypeSystem];
            [control setTitle:labels[index] forState:UIControlStateNormal];
            control.tag=bits[index];
            control.enabled=NO;
            control.accessibilityIdentifier=[@"hades-input-" stringByAppendingString:labels[index]];
            [control addTarget:self action:@selector(controlDown:) forControlEvents:UIControlEventTouchDown];
            [control addTarget:self action:@selector(controlUp:)
                forControlEvents:UIControlEventTouchUpInside|UIControlEventTouchUpOutside|UIControlEventTouchCancel];
            [items addObject:control];[controls addObject:control];
        }
        UIStackView *rowView=[[UIStackView alloc] initWithArrangedSubviews:items];
        rowView.axis=UILayoutConstraintAxisHorizontal;
        rowView.distribution=UIStackViewDistributionFillEqually;
        [rowView.heightAnchor constraintEqualToConstant:36].active=YES;
        [views addObject:rowView];
    }
    self.inputButtons=controls;
#endif
    [views addObjectsFromArray:@[refresh,export,resume,self.logView]];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:views];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 8;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    self.diagnosticStack=stack;
    UIScrollView *scroll=[UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:scroll];
    [scroll addSubview:stack];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12],
        [scroll.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-12],
        [scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
        [self.logView.heightAnchor constraintEqualToConstant:220]]];
    [self refreshLog];
}

- (void)refreshLog {
    fflush(stdout);
    fflush(stderr);
    NSError *error = nil;
    NSFileHandle *file=[NSFileHandle fileHandleForReadingFromURL:[NSURL fileURLWithPath:logPath] error:&error];
    if (!file) {
        self.logView.text=[NSString stringWithFormat:@"无法读取诊断日志：%@",error.localizedDescription];
        return;
    }
    const NSUInteger limit=128*1024;
    unsigned long long end=0;
    BOOL readable=[file seekToEndReturningOffset:&end error:&error];
    unsigned long long offset=end>limit ? end-limit : 0;
    if (readable)
        readable=[file seekToOffset:offset error:&error];
    NSData *data=readable ? [file readDataUpToLength:limit error:&error] : nil;
    NSError *closeError=nil;
    if (![file closeAndReturnError:&closeError] && !error)
        error=closeError;
    if (!data || error) {
        self.logView.text=[NSString stringWithFormat:@"无法读取诊断日志：%@",error.localizedDescription];
        return;
    }
    if (offset && data.length) {
        const unsigned char *bytes=data.bytes;
        NSUInteger start=0;
        while (start<data.length && bytes[start]!='\n')
            start++;
        if (start<data.length)
            data=[data subdataWithRange:NSMakeRange(start+1,data.length-start-1)];
    }
    NSString *text=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    self.logView.text=text ? [NSString stringWithFormat:@"%@%@",offset ? @"仅显示日志末尾 128 KiB；导出包含完整日志。\n" : @"",text]
                          : @"诊断日志末尾不是有效 UTF-8；可导出原始日志。";
}

- (void)exportLog:(UIButton *)sender {
    [self refreshLog];
    UIActivityViewController *sheet = [[UIActivityViewController alloc]
        initWithActivityItems:@[[NSURL fileURLWithPath:logPath]] applicationActivities:nil];
    sheet.popoverPresentationController.sourceView = sender;
    sheet.popoverPresentationController.sourceRect = sender.bounds;
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)runMode:(unsigned)mode {
    if (self.running)
        return;
#ifdef HADES_VULKAN_HOST
    if (mode==3) {
        [self installGameLayout];
        [self.view layoutIfNeeded];
        CAMetalLayer *layer=(CAMetalLayer *)self.surface.layer;
        layer.contentsScale=1280.0/self.surface.bounds.size.width;
        layer.drawableSize=CGSizeMake(1280,720);
        if ([NSBundle.mainBundle.infoDictionary[@"HadesVulkanPixelCaptureProbe"] boolValue]) {
            NSURL *documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                inDomains:NSUserDomainMask].firstObject;
            NSURL *directory=[documents URLByAppendingPathComponent:
                [@"HadesRenderProbe-" stringByAppendingString:NSUUID.UUID.UUIDString] isDirectory:YES];
            NSError *error=nil;
            if (!directory || ![NSFileManager.defaultManager createDirectoryAtURL:directory
                withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:&error]
                || hades_vulkan_configure_capture(directory.path.fileSystemRepresentation,600)) {
                fprintf(stderr,"[Hades-iOS] Cannot configure explicit private GPU pixel capture: %s\n",
                    error.localizedDescription.UTF8String);
                [self showDiagnostics];
                return;
            }
            layer.framebufferOnly=NO;
            fprintf(stderr,"[Hades-iOS] PIXEL_CAPTURE directory=%s; four frame-end snapshots, changes timing, not a fix\n",
                directory.lastPathComponent.UTF8String);
        }
        if (hades_vulkan_configure_surface((__bridge void *)layer)) {
            fprintf(stderr,"[Hades-iOS] Cannot configure actual UIKit Metal surface; no launch\n");
            [self showDiagnostics];
            return;
        }
        if (hades_ipc_configure_input_frontend()) {
            fprintf(stderr,"[Hades-iOS] Cannot register actual touch-controller frontend\n");
            [self showDiagnostics];
            return;
        }
        self.inputConfigured=YES;
        for (UIButton *button in self.inputButtons)
            button.enabled=YES;
    }
#endif
    self.running = YES;
    self.activeMode=mode;
    for (UIButton *button in self.runButtons)
        button.enabled = NO;
    fprintf(stderr, "\n[Hades-iOS] TEST_BEGIN mode=%u; no verified device game entry or onscreen game frame\n", mode);
    [self refreshLog];
    HadesWork *work = calloc(1, sizeof *work);
    if (!work) {
        fprintf(stderr, "[Hades-iOS] Cannot allocate native worker context\n");
        [self showDiagnostics];
        return;
    }
    work->mode = mode;
    work->controller = (__bridge_retained void *)self;
    pthread_attr_t attributes;
    int status = pthread_attr_init(&attributes);
    if (!status) {
        status = pthread_attr_setstacksize(&attributes, 64ull << 20);
        if (!status)
            status = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        if (!status)
            status = pthread_create(&thread, &attributes, probe_thread, work);
        int destroyed = pthread_attr_destroy(&attributes);
        if (destroyed)
            fprintf(stderr, "[Hades-iOS] Cannot destroy worker attributes: %d\n", destroyed);
    }
    if (status) {
        (void)CFBridgingRelease(work->controller);
        free(work);
        fprintf(stderr, "[Hades-iOS] Cannot start native worker: %d\n", status);
        [self showDiagnostics];
    }
}

- (void)executeMode:(unsigned)mode {
    NSString *resources = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"HadesData"];
    hades_log_memory("test-begin",0,0,0);
    int status;
    if (mode == 1) {
        NSString *msl = [resources stringByAppendingPathComponent:@"MSL"];
        NSString *catalog = [resources stringByAppendingPathComponent:@"bindings.json"];
        const char *argv[] = {"render-probe", msl.fileSystemRepresentation, catalog.fileSystemRepresentation};
        status = hades_run_render_probe(3, argv);
    } else {
        NSString *romfs = [resources stringByAppendingPathComponent:@"RomFS"];
        NSString *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                                  inDomains:NSUserDomainMask].firstObject.path;
        NSString *storage = [documents stringByAppendingPathComponent:@"HadesState"];
        if (mode >= 2) {
            NSError *error = nil;
            if (![NSFileManager.defaultManager createDirectoryAtPath:storage withIntermediateDirectories:YES
                                                         attributes:@{NSFilePosixPermissions: @0700} error:&error]) {
                fprintf(stderr, "[Hades-iOS] Cannot create private state directory: %s\n", error.localizedDescription.UTF8String);
                dispatch_async(dispatch_get_main_queue(), ^{ [self showDiagnostics]; });
                return;
            }
        }
        if (mode == 3) {
            romfs = [documents stringByAppendingPathComponent:@"RomFS"];
            fprintf(stderr, "[Hades-iOS] Checking full USB RomFS; no fallback to incomplete embedded resources\n");
            if (!verify_romfs(romfs, [resources stringByAppendingPathComponent:@"RomFS-files.json"])) {
                fprintf(stderr, "[Hades-iOS] Game test rejected: missing/invalid Documents/RomFS; no game launch\n");
                dispatch_async(dispatch_get_main_queue(), ^{ [self showDiagnostics]; });
                return;
            }
        }
        NSString *nrr = [romfs stringByAppendingPathComponent:@".nrr/game.nrr"];
        NSString *nro = [romfs stringByAppendingPathComponent:@"nro/EOSSDK-Switch-Shipping.nro"];
        const char *argv[] = {"hades-core", nrr.fileSystemRepresentation, nro.fileSystemRepresentation,
#ifdef HADES_VULKAN_HOST
                             mode == 3 ? "--game-vulkan" : "--sdk-host",
#else
                             mode == 3 ? "--game-host" : "--sdk-host",
#endif
                             "2048", "--storage", storage.fileSystemRepresentation};
        status = hades_run_core_probe(mode >= 2 ? 7 : 3, argv);
    }
    fprintf(stderr, "[Hades-iOS] TEST_RETURN mode=%u status=%d; not proof of complete game compatibility\n", mode, status);
    hades_log_memory("test-return",0,0,0);
    dispatch_async(dispatch_get_main_queue(), ^{
        [self refreshLog];
        if (mode==3 && status)
            [self showDiagnostics];
        /* Live SDK state remains on its dedicated pthread until process exit. */
        if (mode < 2 && !status) {
            self.running = NO;
            for (UIButton *button in self.runButtons)
                button.enabled = YES;
        }
    });
}
- (void)runCore { [self runMode:0]; }
- (void)runGPU { [self runMode:1]; }
- (void)runSDK { [self runMode:2]; }
- (void)runGame { [self runMode:3]; }
@end

@interface HadesSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation HadesSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class])
        return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [HadesViewController new];
    [self.window makeKeyAndVisible];
}
- (void)sceneWillResignActive:(UIScene *)scene {
    (void)scene;
    HadesViewController *controller=(HadesViewController *)self.window.rootViewController;
    [controller releaseControls];
    if (controller.running && controller.activeMode>=2 && hades_ipc_set_focus_state(2))
        fprintf(stderr,"[Hades-iOS] Inactive foreground focus not delivered; IPC not ready or policy rejected\n");
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    HadesViewController *controller=(HadesViewController *)self.window.rootViewController;
    [controller runRequestedProbe];
    if (controller.running && controller.activeMode>=2 && hades_ipc_set_focus_state(1))
        fprintf(stderr,"[Hades-iOS] Active foreground focus not delivered; IPC not ready or policy rejected\n");
}
- (void)sceneDidEnterBackground:(UIScene *)scene {
    (void)scene;
    HadesViewController *controller=(HadesViewController *)self.window.rootViewController;
    if (hades_ios_background_requires_exit(controller.running,controller.activeMode)) {
        fprintf(stderr,"[Hades-iOS] FOREGROUND_ONLY_ABORT mode=%u: required guest-thread/GPU suspension unimplemented; no background execution, pause or save success claimed\n",controller.activeMode);
        fflush(stdout);
        fflush(stderr);
        _Exit(75);
    }
    fprintf(stderr,"[Hades-iOS] Idle/core-only background observed; no active GPU/SDK execution\n");
}
@end

@interface HadesAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation HadesAppDelegate
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                               inDomains:NSUserDomainMask].firstObject;
        logPath = [[documents URLByAppendingPathComponent:@"hades-device.log"] path];
        if (!documents || !freopen(logPath.fileSystemRepresentation, "a", stdout)
            || !freopen(logPath.fileSystemRepresentation, "a", stderr)) {
            NSLog(@"Cannot open persistent Hades diagnostic log");
            return 2;
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        char model[128] = {0};
        size_t size = sizeof model;
        if (sysctlbyname("hw.machine", model, &size, NULL, 0))
            snprintf(model, sizeof model, "unknown (sysctl failed)");
        NSString *identity=NSBundle.mainBundle.infoDictionary[@"HadesBuildReceipt"];
        NSCharacterSet *hex=[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"];
        if (![identity isKindOfClass:NSString.class] || identity.length!=64
            || [identity rangeOfCharacterFromSet:hex.invertedSet].location!=NSNotFound) {
            fprintf(stderr,"[Hades-iOS] Missing or invalid immutable build receipt; no diagnostic launch\n");
            return 2;
        }
        fprintf(stderr, "\n[Hades-iOS] APP_LAUNCH build=%s receipt=%s device=%s os=%s available-memory=%llu; diagnostic build, not playable\n",
                [NSBundle.mainBundle.infoDictionary[@"CFBundleVersion"] UTF8String],identity.UTF8String,
                model, UIDevice.currentDevice.systemVersion.UTF8String,
                (unsigned long long)os_proc_available_memory());
        hades_log_memory("app-launch",0,0,0);
        id directoryProbe=NSBundle.mainBundle.infoDictionary[@"HadesDirectoryOrderProbe"];
        if (directoryProbe!=(__bridge id)kCFBooleanTrue && directoryProbe!=(__bridge id)kCFBooleanFalse) {
            fprintf(stderr,"[Hades-iOS] Missing or invalid explicit directory order probe setting; no launch\n");
            return 2;
        }
        if (setenv("HADES_FS_DIRECTORY_ORDER_PROBE",[directoryProbe boolValue] ? "1" : "0",1)) {
            fprintf(stderr,"[Hades-iOS] Cannot configure explicit directory order probe\n");
            return 2;
        }
#ifdef HADES_VULKAN_HOST
        NSString *bindingMode=NSBundle.mainBundle.infoDictionary[@"HadesVulkanBindingMode"];
        if (![bindingMode isKindOfClass:NSString.class]
            || (![bindingMode isEqual:@"direct"] && ![bindingMode isEqual:@"argument-buffers"])) {
            fprintf(stderr,"[Hades-iOS] Missing or invalid explicit Vulkan binding mode; no launch\n");
            return 2;
        }
        if (setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS",[bindingMode isEqual:@"direct"] ? "0" : "1",1)) {
            fprintf(stderr,"[Hades-iOS] Cannot configure explicit Vulkan resource binding mode\n");
            return 2;
        }
        fprintf(stderr,"[Hades-iOS] VULKAN_BINDING_EXPERIMENT mode=%s; pixel correctness and flicker require device comparison\n",
            bindingMode.UTF8String);
        id completionProbe=NSBundle.mainBundle.infoDictionary[@"HadesVulkanQueueCompletionProbe"];
        if (completionProbe!=(__bridge id)kCFBooleanTrue && completionProbe!=(__bridge id)kCFBooleanFalse) {
            fprintf(stderr,"[Hades-iOS] Missing or invalid explicit queue completion probe setting; no launch\n");
            return 2;
        }
        if (setenv("HADES_VULKAN_QUEUE_COMPLETION_PROBE",[completionProbe boolValue] ? "1" : "0",1)) {
            fprintf(stderr,"[Hades-iOS] Cannot configure explicit queue completion probe\n");
            return 2;
        }
        id pixelProbe=NSBundle.mainBundle.infoDictionary[@"HadesVulkanPixelCaptureProbe"];
        if (pixelProbe!=(__bridge id)kCFBooleanTrue && pixelProbe!=(__bridge id)kCFBooleanFalse) {
            fprintf(stderr,"[Hades-iOS] Missing or invalid explicit pixel capture probe setting; no launch\n");
            return 2;
        }
        id loadProbe=NSBundle.mainBundle.infoDictionary[@"HadesVulkanAttachmentLoadProbe"];
        if (loadProbe!=(__bridge id)kCFBooleanTrue && loadProbe!=(__bridge id)kCFBooleanFalse) {
            fprintf(stderr,"[Hades-iOS] Missing or invalid explicit attachment load probe setting; no launch\n");
            return 2;
        }
        if (setenv("HADES_VULKAN_ATTACHMENT_LOAD_PROBE",[loadProbe boolValue] ? "1" : "0",1)) {
            fprintf(stderr,"[Hades-iOS] Cannot configure explicit attachment load probe\n");
            return 2;
        }
        id passProbe=NSBundle.mainBundle.infoDictionary[@"HadesVulkanPassCaptureProbe"];
        if (passProbe!=(__bridge id)kCFBooleanTrue && passProbe!=(__bridge id)kCFBooleanFalse) {
            fprintf(stderr,"[Hades-iOS] Missing or invalid explicit pass capture probe setting; no launch\n");
            return 2;
        }
        if (setenv("HADES_VULKAN_PASS_CAPTURE_FRAME",[passProbe boolValue] ? "1800" : "0",1)) {
            fprintf(stderr,"[Hades-iOS] Cannot configure explicit pass capture probe\n");
            return 2;
        }
#endif
        if (hades_ios_requested_probe(argc,argv,&requestedProbe)) {
            fprintf(stderr,"[Hades-iOS] Invalid explicit probe: use --hades-probe core or gpu once; no launch\n");
            return 2;
        }
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(HadesAppDelegate.class));
    }
}
