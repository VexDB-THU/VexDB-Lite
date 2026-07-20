#include "vexfs_fskit_state.h"

#import <Foundation/Foundation.h>
#import <objc/message.h>

#include <dispatch/dispatch.h>
#include <dlfcn.h>

#include <cstdlib>
#include <cstdio>

extern "C" int vexfs_fskit_extension_state(const char *bundle_identifier,
                                             char *url,
                                             std::size_t url_size) {
    if (bundle_identifier == nullptr) return -1;
    if (url != nullptr && url_size > 0) url[0] = '\0';

    @autoreleasepool {
        static void *fskit_framework = dlopen(
            "/System/Library/Frameworks/FSKit.framework/FSKit", RTLD_LAZY | RTLD_LOCAL);
        if (fskit_framework == nullptr) return -1;
        Class client_class = NSClassFromString(@"FSClient");
        SEL shared_selector = NSSelectorFromString(@"sharedInstance");
        SEL fetch_selector =
            NSSelectorFromString(@"fetchInstalledExtensionsWithCompletionHandler:");
        if (client_class == Nil || ![client_class respondsToSelector:shared_selector]) return -1;

        using SendNoArguments = id (*)(id, SEL);
        id client = reinterpret_cast<SendNoArguments>(objc_msgSend)(client_class,
                                                                     shared_selector);
        if (client == nil || ![client respondsToSelector:fetch_selector]) return -1;

        dispatch_semaphore_t finished = dispatch_semaphore_create(0);
        __block NSArray *extensions = nil;
        __block NSError *fetch_error = nil;
        void (^completion)(NSArray *, NSError *) = ^(NSArray *values, NSError *error) {
            extensions = values;
            fetch_error = error;
            dispatch_semaphore_signal(finished);
        };

        using SendCompletion = void (*)(id, SEL, id);
        reinterpret_cast<SendCompletion>(objc_msgSend)(client, fetch_selector, completion);
        const long wait_result = dispatch_semaphore_wait(
            finished, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        if (wait_result != 0) return -2;
        if (fetch_error != nil) {
            if (std::getenv("VEXFS_DEBUG") != nullptr) {
                std::fprintf(stderr, "vexfs: FSClient error=%s\n",
                             fetch_error.localizedDescription.UTF8String);
            }
            return -3;
        }
        if (extensions == nil) return -4;

        NSString *wanted = [NSString stringWithUTF8String:bundle_identifier];
        for (id extension in extensions) {
            NSString *identifier = [extension valueForKey:@"bundleIdentifier"];
            if (![identifier isEqualToString:wanted]) continue;

            NSURL *module_url = [extension valueForKey:@"url"];
            if (url != nullptr && url_size > 0 && module_url != nil) {
                std::snprintf(url, url_size, "%s", module_url.path.UTF8String);
            }
            NSNumber *enabled = [extension valueForKey:@"enabled"];
            return enabled.boolValue ? 2 : 1;
        }
        return 0;
    }
}
