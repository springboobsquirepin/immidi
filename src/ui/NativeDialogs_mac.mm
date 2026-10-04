// macOS implementation of the native file dialogs (NSOpenPanel / NSSavePanel).
#include "NativeDialogs.h"

#import <Cocoa/Cocoa.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"  // allowedFileTypes: fine for plain extensions

namespace immidi {

static NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

bool runNativeDialog(const DialogRequest& req, void* parentWindow, std::vector<std::string>& out) {
    @autoreleasepool {
        NSMutableArray* types = [NSMutableArray array];
        for (const DialogFilter& f : req.filters)
            for (const std::string& e : f.extensions) [types addObject:ns(e)];
        NSModalResponse r;
        if (req.kind == DialogKind::SaveFile) {
            NSSavePanel* panel = [NSSavePanel savePanel];
            [panel setTitle:ns(req.title)];
            if (!req.defaultName.empty()) [panel setNameFieldStringValue:ns(req.defaultName)];
            if (!req.startDir.empty()) [panel setDirectoryURL:[NSURL fileURLWithPath:ns(req.startDir)]];
            if ([types count]) [panel setAllowedFileTypes:types];
            [panel setAllowsOtherFileTypes:YES];
            r = [panel runModal];
            if (r == NSModalResponseOK && [panel URL]) out.push_back([[[panel URL] path] UTF8String]);
        } else {
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            [panel setTitle:ns(req.title)];
            bool folder = req.kind == DialogKind::SelectFolder;
            [panel setCanChooseFiles:!folder];
            [panel setCanChooseDirectories:folder];
            [panel setAllowsMultipleSelection:req.kind == DialogKind::OpenFiles];
            if (!req.startDir.empty()) [panel setDirectoryURL:[NSURL fileURLWithPath:ns(req.startDir)]];
            if (!folder && [types count]) [panel setAllowedFileTypes:types];
            r = [panel runModal];
            if (r == NSModalResponseOK)
                for (NSURL* url in [panel URLs]) out.push_back([[url path] UTF8String]);
        }
        (void)parentWindow;
        NSWindow* keyWindow = parentWindow ? (__bridge NSWindow*)parentWindow : nil;
        [keyWindow makeKeyAndOrderFront:nil];
    }
    return true;
}

} // namespace immidi

#pragma clang diagnostic pop
