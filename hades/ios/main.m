#import <UIKit/UIKit.h>
#include "../runtime/probes.h"
#include "../runtime/ipc.h"
#include "../runtime/diagnostics.h"
#include "policy.h"
#include "joystick.h"
#include "fps.h"
#include "monitor.h"
#include "haptics.h"
#include "controls.h"
#include "action_button.h"
#include "../runtime/display.h"
#include "../runtime/core.h"
#include <CommonCrypto/CommonDigest.h>
#include <os/proc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>
#ifdef HADES_VULKAN_HOST
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include "../runtime/vulkan.h"
@interface HadesSurfaceView : UIView
@end
@implementation HadesSurfaceView
+ (Class)layerClass { return CAMetalLayer.class; }
@end
#endif

static NSString *logPath;
static int requestedProbe=-1;
static uint32_t renderWidth=1280,renderHeight=720;

@interface HadesJoystick : UIControl
@property(nonatomic, strong) UIView *thumb;
@property(nonatomic, strong) UITouch *trackedTouch;
@property(nonatomic) hades_joystick_state stick;
- (void)reset;
@end

@implementation HadesJoystick
- (instancetype)initWithFrame:(CGRect)frame {
    self=[super initWithFrame:frame];
    if (self) {
        self.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:.3];
        self.layer.borderWidth=2;
        self.layer.borderColor=[UIColor.whiteColor colorWithAlphaComponent:.3].CGColor;
        self.exclusiveTouch=NO;
        self.multipleTouchEnabled=NO;
        self.accessibilityIdentifier=@"hades-input-joystick";
        self.accessibilityLabel=@"移动摇杆";
        self.thumb=[UIView new];
        self.thumb.userInteractionEnabled=NO;
        self.thumb.backgroundColor=[UIColor.whiteColor colorWithAlphaComponent:.45];
        self.thumb.layer.borderWidth=2;
        self.thumb.layer.borderColor=[UIColor.whiteColor colorWithAlphaComponent:.7].CGColor;
        [self addSubview:self.thumb];
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CGFloat diameter=MIN(self.bounds.size.width,self.bounds.size.height);
    CGFloat thumbSize=diameter*.42;
    CGFloat radius=(diameter-thumbSize)/2;
    self.layer.cornerRadius=diameter/2;
    self.thumb.bounds=CGRectMake(0,0,thumbSize,thumbSize);
    self.thumb.layer.cornerRadius=thumbSize/2;
    self.thumb.center=CGPointMake(CGRectGetMidX(self.bounds)+self.stick.thumb_x*radius,
                                 CGRectGetMidY(self.bounds)-self.stick.thumb_y*radius);
}
- (void)updateTouch:(UITouch *)touch {
    CGPoint point=[touch locationInView:self];
    CGFloat radius=MIN(self.bounds.size.width,self.bounds.size.height)*.29;
    if (radius<=0)
        return;
    self.stick=hades_joystick_position((point.x-CGRectGetMidX(self.bounds))/radius,
                                      (CGRectGetMidY(self.bounds)-point.y)/radius);
    [self setNeedsLayout];
    [self sendActionsForControlEvents:UIControlEventValueChanged];
}
- (void)reset {
    self.trackedTouch=nil;
    self.stick=(hades_joystick_state){0};
    [self setNeedsLayout];
}
- (void)setEnabled:(BOOL)enabled {
    BOOL changed=!enabled && (self.trackedTouch || self.stick.x || self.stick.y);
    [super setEnabled:enabled];
    if (!enabled)
        [self reset];
    if (changed)
        [self sendActionsForControlEvents:UIControlEventValueChanged];
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.enabled || self.trackedTouch)
        return;
    self.trackedTouch=touches.anyObject;
    [self updateTouch:self.trackedTouch];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (self.trackedTouch && [touches containsObject:self.trackedTouch])
        [self updateTouch:self.trackedTouch];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (self.trackedTouch && [touches containsObject:self.trackedTouch]) {
        [self reset];
        [self sendActionsForControlEvents:UIControlEventValueChanged];
    }
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self touchesEnded:touches withEvent:event];
}
@end

@interface HadesViewController : UIViewController
@property(nonatomic, strong) HadesHaptics *haptics;
@property(nonatomic, strong) UITextView *logView;
@property(nonatomic, strong) NSArray<UIButton *> *runButtons;
@property(nonatomic) BOOL running;
@property(nonatomic) unsigned activeMode;
@property(nonatomic, strong) NSArray<UIButton *> *inputButtons;
@property(nonatomic, strong) HadesJoystick *joystick;
@property(nonatomic, strong) UIView *gameContainer;
@property(nonatomic, strong) NSLayoutConstraint *surfaceHeight;
@property(nonatomic, strong) UIStackView *diagnosticStack;
@property(nonatomic, strong) UIButton *diagnosticButton;
@property(nonatomic, strong) UILabel *fpsLabel;
@property(nonatomic, strong) NSTimer *fpsTimer;
@property(nonatomic) hades_fps_counter fpsCounter;
@property(nonatomic, strong) UIView *systemMonitor;
@property(nonatomic, strong) UIButton *monitorClose;
@property(nonatomic, strong) UITextView *monitorText;
@property(nonatomic) hades_cpu_counter cpuCounter;
@property(nonatomic) double monitorSampleTime;
@property(nonatomic) BOOL inputConfigured;
@property(nonatomic) BOOL actionHitAreaChecked;
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
- (void)dealloc {
    [self.fpsTimer invalidate];
}
- (void)resetFrameRate {
    self.fpsCounter=(hades_fps_counter){0};
    self.fpsLabel.text=@"FPS --";
    self.cpuCounter=(hades_cpu_counter){0};
    self.monitorSampleTime=0;
    self.monitorText.text=@"等待采样…";
}
- (void)toggleSystemMonitor {
    [self.haptics trigger:self.fpsLabel];
    self.systemMonitor.hidden=!self.systemMonitor.hidden;
    self.fpsLabel.accessibilityValue=self.systemMonitor.hidden ? @"监控已关闭" : @"监控已展开";
    self.cpuCounter=(hades_cpu_counter){0};
    self.monitorSampleTime=0;
    self.monitorText.text=@"等待采样…";
    if (!self.systemMonitor.hidden)
        [self updateSystemMonitor];
}
- (void)updateSystemMonitor {
    if (self.systemMonitor.hidden)
        return;
    double now=CACurrentMediaTime();
    if (self.monitorSampleTime && now-self.monitorSampleTime<1.0)
        return;
    self.monitorSampleTime=now;
    double seconds=0,percent=0;
    NSString *cpu=@"采样中";
    if (hades_cpu_seconds(&seconds)) {
        self.cpuCounter=(hades_cpu_counter){0};
        cpu=@"不可用";
    } else {
        int result=hades_cpu_sample(&_cpuCounter,seconds,now,&percent);
        if (result==1)
            cpu=[NSString stringWithFormat:@"%.1f%%",percent];
        else if (result<0) {
            fprintf(stderr,"[Hades-iOS] Invalid CPU monitor sample; no fabricated utilization\n");
            cpu=@"不可用";
        }
    }
    const double mib=1024.0*1024.0;
    hades_memory_observation memory;
    NSString *memoryText=@"内存占用 / 驻留：不可用";
    if (!hades_observe_memory(&memory))
        memoryText=[NSString stringWithFormat:@"内存占用：%.0f MiB（footprint）\n驻留内存：%.0f MiB",
                    memory.physical_footprint/mib,memory.resident/mib];
    NSString *metal=@"不可用";
#ifdef HADES_VULKAN_HOST
    id<MTLDevice> device=((CAMetalLayer *)self.surface.layer).device;
    if (device)
        metal=[NSString stringWithFormat:@"%.0f MiB",device.currentAllocatedSize/mib];
#endif
    NSString *thermal=@"未知";
    switch (NSProcessInfo.processInfo.thermalState) {
        case NSProcessInfoThermalStateNominal:thermal=@"正常";break;
        case NSProcessInfoThermalStateFair:thermal=@"温热";break;
        case NSProcessInfoThermalStateSerious:thermal=@"较高";break;
        case NSProcessInfoThermalStateCritical:thermal=@"严重";break;
        default:break;
    }
    self.monitorText.text=[NSString stringWithFormat:
        @"%@（呈现提交）\nCPU：%@（100%% = 1 核）\n%@\nMetal 分配：%@\nApp 可用余量：%.0f MiB\n系统温度状态：%@\n仅当前 App；内存账目重叠，不能相加。",
        self.fpsLabel.text,cpu,memoryText,metal,os_proc_available_memory()/mib,thermal];
}
- (void)updateFrameRate {
#ifdef HADES_VULKAN_HOST
    if (self.gameContainer.hidden || UIApplication.sharedApplication.applicationState!=UIApplicationStateActive) {
        [self resetFrameRate];
        return;
    }
    hades_fps_counter counter=self.fpsCounter;
    double fps=0;
    int result=hades_fps_sample(&counter,hades_vulkan_present_count(),CACurrentMediaTime(),&fps);
    self.fpsCounter=counter;
    if (result==1)
        self.fpsLabel.text=[NSString stringWithFormat:@"FPS %.1f",fps];
    else if (result<0 || result==2) {
        fprintf(stderr,"[Hades-iOS] FPS_SAMPLE_RESET reason=%s; no fabricated frame rate\n",
                result<0 ? "invalid monotonic timestamp" : "presentation counter restarted");
        [self resetFrameRate];
    }
    [self updateSystemMonitor];
#endif
}
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
        if (bounds.size.width<=0 || bounds.size.height<=0)
            return;
        hades_display_rect frame;
        if (hades_display_layout(bounds.size.width,bounds.size.height,renderWidth,renderHeight,&frame)) {
            fprintf(stderr,"[Hades-iOS] Invalid game display layout\n");
            abort();
        }
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        self.surface.frame=CGRectMake(frame.x,frame.y,frame.width,frame.height);
        CAMetalLayer *layer=(CAMetalLayer *)self.surface.layer;
        layer.contentsScale=(CGFloat)renderWidth/frame.width;
        if (!CGSizeEqualToSize(layer.drawableSize,CGSizeMake(renderWidth,renderHeight)))
            layer.drawableSize=CGSizeMake(renderWidth,renderHeight);
        [CATransaction commit];
        const CGFloat positions[8][2]={{.86,.78},{.92,.89},{.92,.67},{.98,.78},
            {.08,.1},{.92,.1},{.32,.79},{.5,.08}};
        UIEdgeInsets inset=self.view.safeAreaInsets;
        CGFloat available=bounds.size.width-inset.left-inset.right;
        CGFloat stickSize=MAX(0,MIN(144,MIN(available,bounds.size.height-inset.top-inset.bottom)-24));
        CGFloat stickX=MAX(inset.left+stickSize/2+12,inset.left+available*.14);
        CGFloat stickY=MIN(bounds.size.height*.78,bounds.size.height-inset.bottom-stickSize/2-12);
        self.joystick.frame=CGRectMake(stickX-stickSize/2,stickY-stickSize/2,stickSize,stickSize);
        for (unsigned i=0;i<self.inputButtons.count;i++) {
            if (i<4) {
                hades_display_rect button;
                if (hades_action_button_layout(bounds.size.width,bounds.size.height,
                    inset.left,inset.top,inset.right,inset.bottom,i,&button)) {
                    fprintf(stderr,"[Hades-iOS] Insufficient safe area for enlarged action buttons\n");
                    abort();
                }
                self.inputButtons[i].frame=CGRectMake(button.x,button.y,button.width,button.height);
                self.inputButtons[i].layer.cornerRadius=button.width/2;
                continue;
            }
            CGFloat x=inset.left+available*positions[i][0];
            x=MAX(inset.left+28,MIN(bounds.size.width-inset.right-28,x));
            CGFloat y=MAX(inset.top+28,MIN(bounds.size.height-inset.bottom-28,bounds.size.height*positions[i][1]));
            if (i==5)
                y=MAX(y,inset.top+72);
            self.inputButtons[i].frame=CGRectMake(x-26,y-26,52,52);
        }
        self.fpsLabel.frame=CGRectMake(bounds.size.width-inset.right-112,inset.top+4,104,44);
        CGFloat monitorWidth=MIN(320,available-24);
        CGFloat monitorHeight=MIN(220,bounds.size.height-inset.top-inset.bottom-128);
        self.systemMonitor.frame=CGRectMake(inset.left+(available-monitorWidth)/2,
            inset.top+64,monitorWidth,monitorHeight);
        self.monitorClose.frame=CGRectMake(8,0,monitorWidth-16,44);
        self.monitorText.frame=CGRectMake(8,44,monitorWidth-16,monitorHeight-48);
        self.diagnosticButton.frame=CGRectMake(bounds.size.width/2-32,
            bounds.size.height-inset.bottom-40,64,32);
        if (!self.actionHitAreaChecked && self.inputConfigured && !self.gameContainer.hidden
            && bounds.size.width>bounds.size.height) {
            const CGFloat samples[13][2]={{.5,.5},{.1,.5},{.9,.5},{.5,.1},{.5,.9},
                {.2,.2},{.8,.2},{.2,.8},{.8,.8},{0,0},{1,0},{0,1},{1,1}};
            unsigned circleHits=0,cornerRejects=0;
            for (unsigned i=0;i<4;i++) {
                UIButton *button=self.inputButtons[i];
                for (unsigned j=0;j<13;j++) {
                    CGPoint point=CGPointMake(button.bounds.origin.x+button.bounds.size.width*samples[j][0],
                        button.bounds.origin.y+button.bounds.size.height*samples[j][1]);
                    UIView *hit=[self.gameContainer hitTest:[button convertPoint:point toView:self.gameContainer]
                        withEvent:nil];
                    if (j<9)
                        circleHits+=hit==button;
                    else
                        cornerRejects+=hit!=button;
                }
            }
            self.actionHitAreaChecked=YES;
            fprintf(stderr,"[Hades-iOS] ACTION_CIRCLE_HIT_TEST circle=%u/36 transparent-corners=%u/16 status=%s; UIKit dispatch only, not physical touch or guest-action proof\n",
                circleHits,cornerRejects,circleHits==36 && cornerRejects==16 ? "passed" : "failed");
        }
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
    [self.gameContainer addSubview:self.joystick];
    for (UIButton *button in self.inputButtons) {
        [(UIStackView *)button.superview removeArrangedSubview:button];
        [button removeFromSuperview];
        button.translatesAutoresizingMaskIntoConstraints=YES;
        button.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:.5];
        button.layer.cornerRadius=26;
        button.tintColor=UIColor.whiteColor;
        if (button.tag<=3) {
            ((HadesActionButton *)button).circularHitArea=YES;
            button.titleLabel.font=[UIFont systemFontOfSize:24 weight:UIFontWeightSemibold];
        }
        [self.gameContainer addSubview:button];
    }
    self.diagnosticButton=[UIButton buttonWithType:UIButtonTypeSystem];
    [self.diagnosticButton setTitle:@"日志" forState:UIControlStateNormal];
    self.diagnosticButton.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:.5];
    self.diagnosticButton.tintColor=UIColor.whiteColor;
    [self.diagnosticButton addTarget:self action:@selector(showDiagnostics) forControlEvents:UIControlEventTouchUpInside];
    [self.haptics bindActivation:self.diagnosticButton];
    [self.gameContainer addSubview:self.diagnosticButton];
    self.fpsLabel=[UILabel new];
    self.fpsLabel.font=[UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightSemibold];
    self.fpsLabel.textColor=UIColor.whiteColor;
    self.fpsLabel.textAlignment=NSTextAlignmentCenter;
    self.fpsLabel.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:.55];
    self.fpsLabel.layer.cornerRadius=6;
    self.fpsLabel.layer.masksToBounds=YES;
    self.fpsLabel.userInteractionEnabled=YES;
    self.fpsLabel.accessibilityIdentifier=@"hades-fps";
    self.fpsLabel.accessibilityLabel=@"游戏帧率";
    self.fpsLabel.accessibilityTraits=UIAccessibilityTraitButton;
    self.fpsLabel.accessibilityHint=@"轻点展开或关闭系统监控；帧率为呈现提交速率，不是屏幕刷新率";
    self.fpsLabel.accessibilityValue=@"监控已关闭";
    [self.fpsLabel addGestureRecognizer:[[UITapGestureRecognizer alloc]
        initWithTarget:self action:@selector(toggleSystemMonitor)]];
    [self.gameContainer addSubview:self.fpsLabel];
    self.systemMonitor=[UIView new];
    self.systemMonitor.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:.8];
    self.systemMonitor.layer.cornerRadius=10;
    self.systemMonitor.hidden=YES;
    self.systemMonitor.accessibilityIdentifier=@"hades-system-monitor";
    self.monitorClose=[UIButton buttonWithType:UIButtonTypeSystem];
    [self.monitorClose setTitle:@"系统监控 · 点击关闭 ×" forState:UIControlStateNormal];
    self.monitorClose.tintColor=UIColor.whiteColor;
    self.monitorClose.accessibilityLabel=@"关闭系统监控";
    [self.monitorClose addTarget:self action:@selector(toggleSystemMonitor) forControlEvents:UIControlEventTouchUpInside];
    [self.systemMonitor addSubview:self.monitorClose];
    self.monitorText=[UITextView new];
    self.monitorText.editable=NO;
    self.monitorText.selectable=NO;
    self.monitorText.backgroundColor=UIColor.clearColor;
    self.monitorText.textColor=UIColor.whiteColor;
    self.monitorText.font=[UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.monitorText.textContainerInset=UIEdgeInsetsZero;
    self.monitorText.accessibilityIdentifier=@"hades-system-monitor-values";
    [self.systemMonitor addSubview:self.monitorText];
    [self.gameContainer insertSubview:self.systemMonitor belowSubview:self.joystick];
    [self resetFrameRate];
    __weak HadesViewController *weakSelf=self;
    self.fpsTimer=[NSTimer timerWithTimeInterval:.5 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        [weakSelf updateFrameRate];
    }];
    [NSRunLoop.mainRunLoop addTimer:self.fpsTimer forMode:NSRunLoopCommonModes];
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
    if (hades_ipc_publish_input(buttons,self.joystick.stick.x,self.joystick.stick.y))
        fprintf(stderr,"[Hades-iOS] Cannot publish touch-controller state\n");
}
- (void)controlDown:(UIButton *)button { button.selected=YES;[self publishControls]; }
- (void)controlUp:(UIButton *)button { button.selected=NO;[self publishControls]; }
- (void)releaseControls {
    for (UIButton *button in self.inputButtons) {
        if ([button isKindOfClass:HadesActionButton.class])
            [(HadesActionButton *)button reset];
        else
            button.selected=NO;
    }
    [self.joystick reset];
    if (self.inputConfigured)
        [self publishControls];
}
- (void)showDiagnostics {
    [self releaseControls];
    [self resetFrameRate];
    self.gameContainer.hidden=YES;
    self.diagnosticStack.hidden=NO;
    [self refreshLog];
}
- (void)showGameSurface {
    if (self.gameContainer) {
        [self resetFrameRate];
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
    [self.haptics bindActivation:button];
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.haptics=[HadesHaptics new];
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
    self.joystick=[[HadesJoystick alloc] initWithFrame:CGRectZero];
    self.joystick.enabled=NO;
    [self.joystick addTarget:self action:@selector(publishControls) forControlEvents:UIControlEventValueChanged];
    NSArray<NSString *> *labels=@[@"A",@"B",@"X",@"Y",@"L",@"R",@"ZL",@"+"];
    const unsigned bits[]={0,1,2,3,6,7,8,10};
    NSMutableArray<UIButton *> *controls=[NSMutableArray new];
    for (unsigned row=0;row<2;row++) {
        NSMutableArray *items=[NSMutableArray new];
        for (unsigned column=0;column<4;column++) {
            unsigned index=row*4+column;
            UIButton *control=index<4 ? [[HadesActionButton alloc] initWithFrame:CGRectZero]
                                      : [UIButton buttonWithType:UIButtonTypeSystem];
            if (index<4)
                [control setTitleColor:UIColor.systemBlueColor forState:UIControlStateNormal];
            [control setTitle:labels[index] forState:UIControlStateNormal];
            control.tag=bits[index];
            control.enabled=NO;
            control.accessibilityIdentifier=[@"hades-input-" stringByAppendingString:labels[index]];
            [control addTarget:self action:@selector(controlDown:) forControlEvents:UIControlEventTouchDown];
            [self.haptics bindPress:control];
            [control addTarget:self action:@selector(controlUp:)
                forControlEvents:UIControlEventTouchUpInside|UIControlEventTouchUpOutside|UIControlEventTouchCancel];
            if (index<4) {
                [control addTarget:self action:@selector(controlDown:) forControlEvents:UIControlEventTouchDragEnter];
                [control addTarget:self action:@selector(controlUp:) forControlEvents:UIControlEventTouchDragExit];
            }
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
        layer.contentsScale=(CGFloat)renderWidth/self.surface.bounds.size.width;
        layer.drawableSize=CGSizeMake(renderWidth,renderHeight);
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
        self.joystick.enabled=YES;
        for (UIButton *button in self.inputButtons)
            button.enabled=YES;
        [self.view setNeedsLayout];
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
        NSString *resolution=NSBundle.mainBundle.infoDictionary[@"HadesRenderResolution"];
        if (![resolution isKindOfClass:NSString.class]
            || strcmp(resolution.UTF8String,hades_render_resolution)
            || hades_display_extent(resolution.UTF8String,&renderWidth,&renderHeight)) {
            fprintf(stderr,"[Hades-iOS] Missing, invalid, or embedded-profile-mismatched render resolution; no launch\n");
            return 2;
        }
        fprintf(stderr,"[Hades-iOS] RENDER_RESOLUTION requested=%ux%u; guest render targets and frame rate require verification\n",
                renderWidth,renderHeight);
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
