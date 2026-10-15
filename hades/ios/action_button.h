#pragma once
#import <UIKit/UIKit.h>
#include "controls.h"
#include <stdio.h>

@interface HadesActionButton : UIButton
@property(nonatomic) BOOL circularHitArea;
@property(nonatomic, strong) UITouch *trackedTouch;
- (void)reset;
@end

@implementation HadesActionButton
- (instancetype)initWithFrame:(CGRect)frame {
    self=[super initWithFrame:frame];
    if (self) {
        self.exclusiveTouch=NO;
        self.multipleTouchEnabled=NO;
        self.titleLabel.userInteractionEnabled=NO;
    }
    return self;
}
- (void)updateCircleAppearance {
    if (!self.circularHitArea)
        return;
    BOOL active=self.enabled && (self.selected || self.highlighted);
    self.backgroundColor=active ? [UIColor.whiteColor colorWithAlphaComponent:.8]
                                : [UIColor.blackColor colorWithAlphaComponent:.5];
    self.layer.borderWidth=active ? 3 : 1.5;
    self.layer.borderColor=[UIColor.whiteColor colorWithAlphaComponent:active ? 1 : .35].CGColor;
    UIColor *text=active ? UIColor.blackColor : UIColor.whiteColor;
    for (NSNumber *state in @[@(UIControlStateNormal),@(UIControlStateHighlighted),
                              @(UIControlStateSelected),@(UIControlStateSelected|UIControlStateHighlighted)])
        [self setTitleColor:text forState:state.unsignedIntegerValue];
}
- (void)setCircularHitArea:(BOOL)circularHitArea {
    _circularHitArea=circularHitArea;
    [self updateCircleAppearance];
}
- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    [self updateCircleAppearance];
}
- (void)setHighlighted:(BOOL)highlighted {
    [super setHighlighted:highlighted];
    [self updateCircleAppearance];
}
- (void)reset {
    self.trackedTouch=nil;
    self.selected=NO;
    self.highlighted=NO;
}
- (void)setEnabled:(BOOL)enabled {
    BOOL release=!enabled && self.circularHitArea && (self.selected || self.highlighted);
    [super setEnabled:enabled];
    if (!enabled && self.circularHitArea)
        [self reset];
    [self updateCircleAppearance];
    if (release)
        [self sendActionsForControlEvents:UIControlEventTouchCancel];
}
- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent *)event {
    if (!self.circularHitArea)
        return [super pointInside:point withEvent:event];
    return hades_action_button_contains(self.bounds.size.width,self.bounds.size.height,
        point.x-self.bounds.origin.x,point.y-self.bounds.origin.y);
}
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event {
    if (!self.circularHitArea)
        return [super hitTest:point withEvent:event];
    if (self.hidden || self.alpha<=.01 || !self.userInteractionEnabled || !self.enabled
        || ![self pointInside:point withEvent:event])
        return nil;
    return self;
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.circularHitArea) {
        [super touchesBegan:touches withEvent:event];
        return;
    }
    UITouch *touch=touches.anyObject;
    if (!self.enabled || self.trackedTouch || !touch
        || ![self pointInside:[touch locationInView:self] withEvent:event])
        return;
    self.trackedTouch=touch;
    self.highlighted=YES;
    [self sendActionsForControlEvents:UIControlEventTouchDown];
    static unsigned loggedPresses;
    if (loggedPresses<16) {
        loggedPresses++;
        CGPoint point=[touch locationInView:self];
        fprintf(stderr,"[Hades-iOS] ACTION_CIRCLE_DOWN bit=%ld local=%.1f/%.1f bounds=%.1f/%.1f selected=%d; first16 presses only\n",
            (long)self.tag,point.x,point.y,self.bounds.size.width,self.bounds.size.height,self.selected);
    }
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.circularHitArea) {
        [super touchesMoved:touches withEvent:event];
        return;
    }
    if (!self.trackedTouch || ![touches containsObject:self.trackedTouch])
        return;
    BOOL inside=[self pointInside:[self.trackedTouch locationInView:self] withEvent:event];
    if (inside!=self.highlighted) {
        self.highlighted=inside;
        [self sendActionsForControlEvents:inside ? UIControlEventTouchDragEnter : UIControlEventTouchDragExit];
    }
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.circularHitArea) {
        [super touchesEnded:touches withEvent:event];
        return;
    }
    if (self.trackedTouch && [touches containsObject:self.trackedTouch]) {
        BOOL inside=[self pointInside:[self.trackedTouch locationInView:self] withEvent:event];
        [self reset];
        [self sendActionsForControlEvents:inside ? UIControlEventTouchUpInside : UIControlEventTouchUpOutside];
    }
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.circularHitArea) {
        [super touchesCancelled:touches withEvent:event];
        return;
    }
    if (self.trackedTouch && [touches containsObject:self.trackedTouch]) {
        [self reset];
        [self sendActionsForControlEvents:UIControlEventTouchCancel];
    }
}
@end
