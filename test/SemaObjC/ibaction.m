// RUN: %clang_cc1 %s -verify

@interface Foo 
{
  __attribute__((iboutlet)) id myoutlet;
}
+ (void) __attribute__((ibaction)) myClassMethod:(id)msg; // expected-warning{{ibaction attribute can only be applied to Objective-C instance methods}}
- (void) __attribute__((ibaction)) myMessage:(id)msg;
@end

@implementation Foo
+ (void) __attribute__((ibaction)) myClassMethod:(id)msg {} // expected-warning{{ibaction attribute can only be applied to Objective-C instance methods}}
// Normally attributes should not be attached to method definitions, but
// we allow 'ibaction' to be attached because it can be expanded from
// the IBAction macro.
- (void) __attribute__((ibaction)) myMessage:(id)msg {} // no-warning
@end
