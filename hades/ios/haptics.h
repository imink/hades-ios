#pragma once
#import <UIKit/UIKit.h>

@interface HadesHaptics : NSObject
@property(nonatomic, strong) UIImpactFeedbackGenerator *feedback;
- (void)bindPress:(UIControl *)control;
- (void)bindActivation:(UIControl *)control;
- (void)trigger:(id)sender;
@end

@implementation HadesHaptics
- (void)bindPress:(UIControl *)control {
    [control addTarget:self action:@selector(trigger:) forControlEvents:UIControlEventTouchDown];
}
- (void)bindActivation:(UIControl *)control {
    [control addTarget:self action:@selector(trigger:) forControlEvents:UIControlEventTouchUpInside];
}
- (void)trigger:(id)sender {
    if (UIApplication.sharedApplication.applicationState!=UIApplicationStateActive)
        return;
    if ([sender isKindOfClass:UIControl.class] && ![(UIControl *)sender isEnabled])
        return;
    if (!self.feedback)
        self.feedback=[[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    [self.feedback prepare];
    [self.feedback impactOccurred];
}
@end
