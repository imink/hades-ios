#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include "probes.h"
#include "vulkan.h"
#include "ipc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@interface HadesProbeSurface : NSView
@property uint64_t pressed;
- (unsigned)buttonForKey:(unsigned)key;
- (void)applyKey:(unsigned)key pressed:(BOOL)pressed;
@end
@implementation HadesProbeSurface
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)publishKeys {
    int32_t x=(self.pressed&(1u<<14) ? 32767 : 0)-(self.pressed&(1u<<12) ? 32767 : 0);
    int32_t y=(self.pressed&(1u<<13) ? 32767 : 0)-(self.pressed&(1u<<15) ? 32767 : 0);
    if (hades_ipc_publish_input(self.pressed&~0xf000ull,x,y)) {
        fprintf(stderr,"[hades-window] Cannot publish actual keyboard state\n");
        abort();
    }
}
- (unsigned)buttonForKey:(unsigned)key {
    switch (key) {
        case 36:return 0; case 53:return 1; case 7:return 2; case 6:return 3;
        case 12:return 6; case 14:return 7; case 49:return 8;
        case 35:return 10; case 123:return 12; case 126:return 13;
        case 124:return 14; case 125:return 15; default:return 64;
    }
}
- (void)keyDown:(NSEvent *)event {
    unsigned bit=[self buttonForKey:event.keyCode];
    if (bit<64) [self applyKey:event.keyCode pressed:YES];
    else [super keyDown:event];
}
- (void)keyUp:(NSEvent *)event {
    unsigned bit=[self buttonForKey:event.keyCode];
    if (bit<64) [self applyKey:event.keyCode pressed:NO];
    else [super keyUp:event];
}
- (void)applyKey:(unsigned)key pressed:(BOOL)pressed {
    unsigned bit=[self buttonForKey:key];
    if (bit>=64) {
        fprintf(stderr,"[hades-window] Invalid explicitly scripted keyboard key\n");
        abort();
    }
    if (pressed) self.pressed|=1ull<<bit;
    else self.pressed&=~(1ull<<bit);
    [self publishKeys];
}
- (BOOL)resignFirstResponder {
    self.pressed=0;
    [self publishKeys];
    return [super resignFirstResponder];
}
@end

typedef struct { int argc; const char *const *argv; } window_work;
static void *run_guest(void *argument) {
    window_work *work=argument;
    @autoreleasepool {
        int result=hades_run_core_probe(work->argc,work->argv);
        fprintf(stderr,"[hades-window] guest probe returned=%d; no inferred gameplay success\n",result);
        exit(result);
    }
}

int hades_run_window_probe(int argc,const char *const *argv) {
    @autoreleasepool {
        NSApplication *application=[NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,960,540)
            styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        window.title=@"Hades local native rendering probe — not iOS gameplay evidence";
        HadesProbeSurface *view=[[HadesProbeSurface alloc] initWithFrame:NSMakeRect(0,0,960,540)];
        view.wantsLayer=YES;
        window.contentView=view;
        CAMetalLayer *layer=(CAMetalLayer *)view.layer;
        layer.contentsScale=1280.0/view.bounds.size.width;
        layer.drawableSize=CGSizeMake(1280,720);
        layer.framebufferOnly=NO;
        [window center];
        [window makeKeyAndOrderFront:nil];
        [application activateIgnoringOtherApps:YES];
        [window displayIfNeeded];
        if (hades_ipc_configure_input_frontend())
            return 2;
        const char *capture=getenv("HADES_CAPTURE_DIR");
        if (capture) {
            const char *interval=getenv("HADES_CAPTURE_STEP");
            char *end=NULL;
            unsigned long step=interval ? strtoul(interval,&end,10) : 1000;
            if ((interval && (!interval[0] || *end)) || step>10000
                || hades_vulkan_configure_capture(capture,(unsigned)step))
                return 2;
        }
        [window makeFirstResponder:view];
        const char *script=getenv("HADES_INPUT_SCRIPT");
        if (script) {
            NSString *text=[NSString stringWithUTF8String:script];
            NSArray<NSString *> *parts=[text componentsSeparatedByString:@","];
            if (!text || !parts.count || parts.count>16)
                return 2;
            NSMutableArray<NSArray<NSNumber *> *> *actions=[NSMutableArray new];
            unsigned long long previous=0;
            for (NSString *part in parts) {
                unsigned long long frame;
                unsigned key,state;
                int consumed=0;
                if (sscanf(part.UTF8String,"%llu:%u:%u%n",&frame,&key,&state,&consumed)!=3
                    || consumed!=(int)strlen(part.UTF8String) || !frame || frame>100000
                    || frame<=previous || state>1 || [view buttonForKey:key]>=64) {
                    fprintf(stderr,"[hades-window] Invalid explicit local input script\n");
                    return 2;
                }
                [actions addObject:@[@(frame),@(key),@(state)]];
                previous=frame;
            }
            __block NSUInteger index=0;
            [NSTimer scheduledTimerWithTimeInterval:0.01 repeats:YES block:^(NSTimer *timer) {
                uint64_t frame=hades_vulkan_present_count();
                while (index<actions.count && actions[index][0].unsignedLongLongValue<=frame) {
                    [view applyKey:actions[index][1].unsignedIntValue pressed:actions[index][2].boolValue];
                    fprintf(stderr,"[hades-window] EXPLICIT_TEST_KEY frame=%llu key=%u pressed=%d\n",
                            (unsigned long long)frame,actions[index][1].unsignedIntValue,actions[index][2].boolValue);
                    index++;
                }
                if (index==actions.count)
                    [timer invalidate];
            }];
        }
        if (hades_vulkan_configure_surface((__bridge void *)layer))
            return 2;
        static window_work work;
        work=(window_work){argc,argv};
        pthread_attr_t attributes;
        int status=pthread_attr_init(&attributes);
        if (status) {
            fprintf(stderr,"[hades-window] Cannot initialize guest worker attributes: %d\n",status);
            return 2;
        }
        status=pthread_attr_setstacksize(&attributes,64*1024*1024);
        pthread_t thread;
        if (!status)
            status=pthread_create(&thread,&attributes,run_guest,&work);
        int cleanup=pthread_attr_destroy(&attributes);
        if (status || cleanup) {
            fprintf(stderr,"[hades-window] Cannot create/finish guest worker attributes: %d/%d\n",status,cleanup);
            return 2;
        }
        fprintf(stderr,"[hades-window] ATTACHED_METAL_WINDOW number=%ld; native worker owns SDK context\n",(long)window.windowNumber);
        [application run];
        return 2;
    }
}
