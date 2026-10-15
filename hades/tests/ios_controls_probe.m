#import "../ios/action_button.h"
#import "../ios/haptics.h"
#include <assert.h>
#include <stdio.h>

@interface HadesProbeFeedback : UIImpactFeedbackGenerator
@property(nonatomic) unsigned impacts;
@property(nonatomic) unsigned preparations;
@end
@implementation HadesProbeFeedback
- (void)prepare { self.preparations++; }
- (void)impactOccurred { self.impacts++; }
@end

@interface HadesProbeTouch : UITouch
@property(nonatomic) CGPoint point;
@property(nonatomic, weak) UIView *host;
@end
@implementation HadesProbeTouch
- (CGPoint)locationInView:(UIView *)view {
    return [self.host convertPoint:self.point toView:view];
}
@end

@interface HadesProbeInput : NSObject
@property(nonatomic, strong) NSArray<HadesActionButton *> *buttons;
@property(nonatomic) uint64_t mask;
- (void)down:(UIButton *)button;
- (void)up:(UIButton *)button;
@end
@implementation HadesProbeInput
- (void)publish {
    self.mask=0;
    for (UIButton *button in self.buttons)
        if (button.selected)
            self.mask|=1ull<<button.tag;
}
- (void)down:(UIButton *)button { button.selected=YES; [self publish]; }
- (void)up:(UIButton *)button { button.selected=NO; [self publish]; }
@end

static HadesProbeTouch *touchAt(HadesActionButton *button, CGPoint point) {
    HadesProbeTouch *touch=[HadesProbeTouch new];
    touch.host=button;
    touch.point=point;
    return touch;
}

static unsigned backgroundPixel(HadesActionButton *button) {
    [button layoutIfNeeded];
    UIGraphicsImageRendererFormat *format=[UIGraphicsImageRendererFormat defaultFormat];
    format.scale=1;
    UIGraphicsImageRenderer *renderer=[[UIGraphicsImageRenderer alloc] initWithSize:button.bounds.size format:format];
    UIImage *image=[renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
        [button.layer renderInContext:context.CGContext];
    }];
    unsigned char pixels[72*72*4]={0};
    CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();
    CGContextRef context=CGBitmapContextCreate(pixels,72,72,8,72*4,color,
        kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
    assert(context);
    CGContextDrawImage(context,CGRectMake(0,0,72,72),image.CGImage);
    CGContextRelease(context);
    CGColorSpaceRelease(color);
    unsigned offset=(36*72+8)*4;
    return pixels[offset]+pixels[offset+1]+pixels[offset+2];
}

@interface HadesControlsProbeDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation HadesControlsProbeDelegate
@end

@interface HadesControlsProbeSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation HadesControlsProbeSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    self.window=[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController=[UIViewController new];
    [self.window makeKeyAndVisible];
    dispatch_async(dispatch_get_main_queue(), ^{
        UIView *container=[[UIView alloc] initWithFrame:CGRectMake(0,0,932,430)];
        [self.window.rootViewController.view addSubview:container];
        NSMutableArray<HadesActionButton *> *buttons=[NSMutableArray new];
        for (unsigned i=0;i<4;i++) {
            HadesActionButton *button=[[HadesActionButton alloc] initWithFrame:CGRectZero];
            assert(button.buttonType==UIButtonTypeCustom);
            button.circularHitArea=YES;
            button.tag=i;
            [button setTitle:@[@"A",@"B",@"X",@"Y"][i] forState:UIControlStateNormal];
            hades_display_rect frame;
            assert(!hades_action_button_layout(932,430,59,0,59,21,i,&frame));
            button.frame=CGRectMake(frame.x,frame.y,frame.width,frame.height);
            button.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:.5];
            button.layer.cornerRadius=36;
            [container addSubview:button];
            [buttons addObject:button];
        }
        [container layoutIfNeeded];
        HadesProbeInput *input=[HadesProbeInput new];
        input.buttons=buttons;
        assert(UIApplication.sharedApplication.applicationState==UIApplicationStateActive);
        HadesHaptics *haptics=[HadesHaptics new];
        HadesProbeFeedback *feedback=[[HadesProbeFeedback alloc] initWithStyle:UIImpactFeedbackStyleLight];
        haptics.feedback=feedback;
        for (HadesActionButton *button in buttons) {
            [haptics bindPress:button];
            [button addTarget:input action:@selector(down:)
                forControlEvents:UIControlEventTouchDown|UIControlEventTouchDragEnter];
            [button addTarget:input action:@selector(up:) forControlEvents:
                UIControlEventTouchUpInside|UIControlEventTouchUpOutside|UIControlEventTouchCancel|UIControlEventTouchDragExit];
        }
        unsigned hits=0,misses=0;
        for (HadesActionButton *button in buttons) {
            for (NSValue *value in @[[NSValue valueWithCGPoint:CGPointMake(36,36)],
                    [NSValue valueWithCGPoint:CGPointMake(6,36)],
                    [NSValue valueWithCGPoint:CGPointMake(66,36)],
                    [NSValue valueWithCGPoint:CGPointMake(36,6)],
                    [NSValue valueWithCGPoint:CGPointMake(36,66)],
                    [NSValue valueWithCGPoint:CGPointMake(12,12)],
                    [NSValue valueWithCGPoint:CGPointMake(60,12)],
                    [NSValue valueWithCGPoint:CGPointMake(12,60)],
                    [NSValue valueWithCGPoint:CGPointMake(60,60)]]) {
                CGPoint point=value.CGPointValue;
                UIView *hit=[container hitTest:[button convertPoint:point toView:container] withEvent:nil];
                BOOL correct=hit==button || [hit isDescendantOfView:button];
                fprintf(stderr,"CIRCLE_HIT local=%.0f/%.0f hit=%s expected=%s correct=%d\n",
                    point.x,point.y,NSStringFromClass(hit.class).UTF8String,
                    NSStringFromClass(button.class).UTF8String,correct);
                if (correct) hits++; else misses++;
                UIColor *normal=button.backgroundColor;
                unsigned normalPixel=backgroundPixel(button);
                HadesProbeTouch *touch=touchAt(button,point);
                unsigned impacts=feedback.impacts;
                [button touchesBegan:[NSSet setWithObject:touch] withEvent:nil];
                assert(feedback.impacts==impacts+1);
                assert(button.selected && button.highlighted && button.trackedTouch==touch);
                assert(input.mask==(1ull<<button.tag));
                assert(![button.backgroundColor isEqual:normal] && button.layer.borderWidth==3);
                assert(backgroundPixel(button)>normalPixel+300);
                [button touchesEnded:[NSSet setWithObject:touch] withEvent:nil];
                assert(feedback.impacts==impacts+1);
                assert(!button.selected && !button.highlighted && !button.trackedTouch && input.mask==0);
                assert([button.backgroundColor isEqual:normal] && button.layer.borderWidth==1.5);
            }
            for (NSValue *corner in @[[NSValue valueWithCGPoint:CGPointMake(0,0)],
                    [NSValue valueWithCGPoint:CGPointMake(72,0)],
                    [NSValue valueWithCGPoint:CGPointMake(0,72)],
                    [NSValue valueWithCGPoint:CGPointMake(72,72)]]) {
                CGPoint point=corner.CGPointValue;
                unsigned impacts=feedback.impacts;
                assert([container hitTest:[button convertPoint:point toView:container] withEvent:nil]!=button);
                [button touchesBegan:[NSSet setWithObject:touchAt(button,point)] withEvent:nil];
                assert(!button.selected && !button.trackedTouch && input.mask==0);
                assert(feedback.impacts==impacts);
            }
            HadesProbeTouch *touch=touchAt(button,CGPointMake(6,36));
            NSSet<UITouch *> *touches=[NSSet setWithObject:touch];
            unsigned impacts=feedback.impacts;
            [button touchesBegan:touches withEvent:nil];
            assert(feedback.impacts==impacts+1);
            touch.point=CGPointMake(0,0);
            [button touchesMoved:touches withEvent:nil];
            assert(!button.selected && !button.highlighted && input.mask==0);
            touch.point=CGPointMake(66,36);
            [button touchesMoved:touches withEvent:nil];
            assert(button.selected && button.highlighted && input.mask==(1ull<<button.tag));
            assert(feedback.impacts==impacts+1);
            HadesProbeTouch *second=touchAt(button,CGPointMake(36,36));
            [button touchesBegan:[NSSet setWithObject:second] withEvent:nil];
            [button touchesEnded:[NSSet setWithObject:second] withEvent:nil];
            assert(feedback.impacts==impacts+1);
            assert(button.trackedTouch==touch && button.selected);
            [button touchesCancelled:touches withEvent:nil];
            assert(feedback.impacts==impacts+1);
            assert(!button.selected && !button.highlighted && !button.trackedTouch && !input.mask);
            [button touchesBegan:touches withEvent:nil];
            touch.point=CGPointMake(72,72);
            [button touchesEnded:touches withEvent:nil];
            assert(!button.selected && !button.highlighted && !button.trackedTouch && !input.mask);
            touch.point=CGPointMake(6,36);
            [button touchesBegan:touches withEvent:nil];
            button.enabled=NO;
            assert(!button.selected && !button.highlighted && !button.trackedTouch && !input.mask);
            assert(![button hitTest:CGPointMake(36,36) withEvent:nil]);
            button.enabled=YES;
            button.hidden=YES;
            assert(![button hitTest:CGPointMake(36,36) withEvent:nil]);
            button.hidden=NO;
            button.alpha=.01;
            assert(![button hitTest:CGPointMake(36,36) withEvent:nil]);
            button.alpha=1;
            button.userInteractionEnabled=NO;
            assert(![button hitTest:CGPointMake(36,36) withEvent:nil]);
            button.userInteractionEnabled=YES;
        }
        HadesProbeTouch *first=touchAt(buttons[0],CGPointMake(6,36));
        HadesProbeTouch *second=touchAt(buttons[1],CGPointMake(66,36));
        [buttons[0] touchesBegan:[NSSet setWithObject:first] withEvent:nil];
        [buttons[1] touchesBegan:[NSSet setWithObject:second] withEvent:nil];
        assert(input.mask==3);
        [buttons[0] touchesEnded:[NSSet setWithObject:first] withEvent:nil];
        assert(input.mask==2);
        [buttons[1] touchesEnded:[NSSet setWithObject:second] withEvent:nil];
        assert(!input.mask);
        for (HadesActionButton *button in buttons) {
            button.circularHitArea=NO;
            assert([button pointInside:CGPointMake(1,1) withEvent:nil]);
        }
        for (unsigned i=0;i<4;i++) {
            UIButton *button=[UIButton buttonWithType:UIButtonTypeSystem];
            [haptics bindPress:button];
            unsigned impacts=feedback.impacts;
            [button sendActionsForControlEvents:UIControlEventTouchDown];
            [button sendActionsForControlEvents:UIControlEventTouchDragInside|UIControlEventTouchDragExit|
                UIControlEventTouchDragEnter|UIControlEventTouchUpInside|UIControlEventTouchCancel];
            assert(feedback.impacts==impacts+1);
        }
        UIButton *activation=[UIButton buttonWithType:UIButtonTypeSystem];
        [haptics bindActivation:activation];
        unsigned impacts=feedback.impacts;
        [activation sendActionsForControlEvents:UIControlEventTouchDown|UIControlEventTouchUpOutside|UIControlEventTouchCancel];
        assert(feedback.impacts==impacts);
        [activation sendActionsForControlEvents:UIControlEventTouchUpInside];
        assert(feedback.impacts==impacts+1);
        activation.enabled=NO;
        [activation sendActionsForControlEvents:UIControlEventTouchUpInside];
        assert(feedback.impacts==impacts+1);
        UIControl *unboundJoystick=[UIControl new];
        [unboundJoystick sendActionsForControlEvents:UIControlEventTouchDown|UIControlEventValueChanged|
            UIControlEventTouchDragInside|UIControlEventTouchUpInside];
        assert(feedback.impacts==impacts+1);
        assert(feedback.preparations==feedback.impacts);
        fprintf(stdout,"UIKIT_HAPTICS_EVENTS_OK: one light request per press or activation, no re-entry/release/cancel repeats, disabled rejection; generator spy, not physical vibration\n");
        fprintf(stdout,"UIKIT_CIRCLE_HITS hits=%u misses=%u\n",hits,misses);
        fprintf(stdout,"UIKIT_CIRCLE_INPUT_OK: whole-circle press/release, active pixels, drag exit/re-entry, cancellation, disable, second finger, simultaneous controls, diagnostic rectangle\n");
        fflush(stdout);
        exit(misses ? 1 : 0);
    });
}
@end

int main(int argc,char **argv) {
    @autoreleasepool {
        return UIApplicationMain(argc,argv,nil,NSStringFromClass(HadesControlsProbeDelegate.class));
    }
}
