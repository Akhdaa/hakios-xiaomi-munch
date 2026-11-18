#import <Foundation/Foundation.h>
#import <dlfcn.h>
#import <stdlib.h>
#import <unistd.h>

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)installApplication:(NSURL *)url withOptions:(NSDictionary *)options error:(NSError **)error;
@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc != 3) {
            fputs("Usage: munch_ipa_install FILE.ipa BUNDLE_ID\n", stderr);
            return 2;
        }
        dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
        Class workspaceClass = NSClassFromString(@"LSApplicationWorkspace");
        LSApplicationWorkspace *workspace = [workspaceClass defaultWorkspace];
        if (![workspace respondsToSelector:@selector(installApplication:withOptions:error:)]) {
            fputs("IPA: LaunchServices installer unavailable\n", stderr);
            return 1;
        }
        NSFileManager *files = NSFileManager.defaultManager;
        char staging[] = "/private/var/tmp/hakios-ipa.XXXXXX";
        if (!mkdtemp(staging)) {
            perror("IPA staging directory");
            return 1;
        }
        NSString *directory = [NSString stringWithUTF8String:staging];
        NSString *copy = [directory stringByAppendingPathComponent:@"Install.ipa"];
        NSError *error = nil;
        BOOL ok = NO;
        @try {
            if ([files copyItemAtPath:[NSString stringWithUTF8String:argv[1]] toPath:copy error:&error]) {
                NSDictionary *options = @{@"CFBundleIdentifier": [NSString stringWithUTF8String:argv[2]]};
                ok = [workspace installApplication:[NSURL fileURLWithPath:copy] withOptions:options error:&error];
            }
            if (!ok) NSLog(@"IPA: installation failed: %@", error);
        } @catch (NSException *exception) {
            NSLog(@"IPA: installation failed: %@", exception.reason);
        } @finally {
            [files removeItemAtPath:directory error:NULL];
        }
        if (ok) printf("IPA: installed %s\n", argv[2]);
        return ok ? 0 : 1;
    }
}
