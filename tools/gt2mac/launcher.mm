// Native first-run disc import and launch. The player app needs no shell/Python/SDK.
#import <Cocoa/Cocoa.h>
#include "launch_protocol.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <spawn.h>
#include <crt_externs.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::string Text(NSString* value) { return value ? std::string(value.UTF8String) : std::string(); }
[[noreturn]] void Fail(NSString* message) { throw std::runtime_error(Text(message)); }
void Alert(NSString* title, NSString* message) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = title; alert.informativeText = message;
    [alert addButtonWithTitle:@"OK"]; [alert runModal];
}
NSString* Run(NSString* executable, NSArray<NSString*>* arguments) {
    NSTask* task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:executable];
    task.arguments = arguments;
    // The helper emits only a short result; file output avoids pipe back-pressure.
    NSString* log = [NSTemporaryDirectory() stringByAppendingPathComponent:[@"gt2-import-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    [NSFileManager.defaultManager createFileAtPath:log contents:nil attributes:nil];
    NSFileHandle* output = [NSFileHandle fileHandleForWritingAtPath:log];
    if (!output) Fail(@"Cannot create import log.");
    task.standardOutput = output; task.standardError = output;
    NSError* error = nil;
    if (![task launchAndReturnError:&error]) { [output closeAndReturnError:nullptr]; [NSFileManager.defaultManager removeItemAtPath:log error:nullptr]; Fail(error.localizedDescription); }
    // Service Cocoa while extraction is running so the progress window stays responsive.
    while (task.running) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    [task waitUntilExit];
    if (![output closeAndReturnError:&error]) Fail(error.localizedDescription);
    NSString* result = [NSString stringWithContentsOfFile:log encoding:NSUTF8StringEncoding error:nullptr];
    [NSFileManager.defaultManager removeItemAtPath:log error:nullptr];
    if (task.terminationStatus != 0) Fail(result ?: @"Disc import failed.");
    return result ?: @"";
}
NSString* DiscPath(NSURL* selection) {
    NSString* path = selection.path;
    if (![path.pathExtension.lowercaseString isEqualToString:@"cue"]) return path;
    NSError* error = nil;
    NSString* cue = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];
    if (!cue) Fail(error.localizedDescription);
    auto matches = [&](NSString* pattern) {
        NSRegularExpression* regex = [NSRegularExpression regularExpressionWithPattern:pattern
            options:NSRegularExpressionCaseInsensitive | NSRegularExpressionAnchorsMatchLines error:nullptr];
        return [regex matchesInString:cue options:0 range:NSMakeRange(0, cue.length)];
    };
    NSArray<NSTextCheckingResult*>* files = matches(@"^\\s*FILE\\s+\"([^\"]+)\"\\s+BINARY\\s*$");
    NSArray<NSTextCheckingResult*>* tracks = matches(@"^\\s*TRACK\\s+([0-9]+)\\s+(\\S+)\\s*$");
    if (files.count != 1 || tracks.count != 1 ||
        ![[cue substringWithRange:[tracks[0] rangeAtIndex:1]] isEqualToString:@"01"] ||
        ![[[cue substringWithRange:[tracks[0] rangeAtIndex:2]] uppercaseString] isEqualToString:@"MODE2/2352"])
        Fail(@"Choose a single-track MODE2/2352 BIN/CUE image. Extract ZIP/7z archives first.");
    NSString* name = [cue substringWithRange:[files[0] rangeAtIndex:1]];
    if (name.isAbsolutePath || [name.pathComponents containsObject:@".."] || [name containsString:@"\\"] || [name containsString:@":"])
        Fail(@"Unsafe file path in CUE.");
    return [path.stringByDeletingLastPathComponent stringByAppendingPathComponent:name];
}
bool HasDisc(const std::filesystem::path& root) {
    for (const char* mode : {"arcade", "simulation"})
        if (std::filesystem::is_regular_file(root / mode / "disc.raw2352") && std::filesystem::is_directory(root / mode / "assets")) return true;
    return false;
}
bool Import(NSString* helper, const std::filesystem::path& root) {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Import your Gran Turismo 2 discs";
    panel.message = @"Select your Arcade and/or Simulation BIN/CUE images (raw 2352-byte sectors). Game data is not included. Existing saves are kept.";
    panel.canChooseDirectories = NO; panel.allowsMultipleSelection = YES;
    if ([panel runModal] != NSModalResponseOK) return false;
    NSMutableArray<NSDictionary*>* inputs = [NSMutableArray array];
    NSMutableSet<NSString*>* modes = [NSMutableSet set];
    // Inspect every input before changing any installed data.
    for (NSURL* url in panel.URLs) {
        NSString* image = DiscPath(url);
        NSString* result = Run(helper, @[@"inspect", image]);
        NSDictionary* info = [NSJSONSerialization JSONObjectWithData:[result dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nullptr];
        NSString* mode = [info isKindOfClass:NSDictionary.class] ? info[@"mode"] : nil;
        if (![mode isEqualToString:@"arcade"] && ![mode isEqualToString:@"simulation"]) Fail(@"Unrecognized game disc.");
        if ([modes containsObject:mode]) Fail(@"Select only one disc for each mode.");
        [modes addObject:mode]; [inputs addObject:@{@"image": image, @"mode": mode}];
    }
    const auto stage = root / (".import-" + Text(NSUUID.UUID.UUIDString));
    std::filesystem::create_directory(stage);
    NSWindow* progress = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 110)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    progress.releasedWhenClosed = NO;
    progress.title = @"Preparing GT2";
    NSTextField* label = [NSTextField labelWithString:@"Copying your discs and extracting game data…"];
    label.frame = NSMakeRect(20, 55, 420, 25); [progress.contentView addSubview:label];
    NSProgressIndicator* spinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 20, 420, 15)];
    spinner.indeterminate = YES; [progress.contentView addSubview:spinner]; [spinner startAnimation:nil];
    [progress center]; [progress makeKeyAndOrderFront:nil];
    std::vector<std::filesystem::path> installed, backedUp;
    const auto backup = root / "backups" / ("discs-" + Text(NSUUID.UUID.UUIDString));
    try {
        for (NSDictionary* input in inputs) {
            const auto out = stage / Text(input[@"mode"]);
            Run(helper, @[@"extract", input[@"image"], [NSString stringWithUTF8String:out.c_str()]]);
        }
        std::filesystem::create_directories(backup);
        for (NSDictionary* input in inputs) {
            const auto mode = std::filesystem::path(Text(input[@"mode"]));
            const auto target = root / mode;
            if (std::filesystem::is_symlink(target)) Fail(@"Refusing to replace a linked game-data directory.");
            if (std::filesystem::exists(target)) {
                std::filesystem::rename(target, backup / mode); backedUp.push_back(mode);
            }
            std::filesystem::rename(stage / mode, target); installed.push_back(mode);
        }
        std::filesystem::remove(stage); // empty directory only; never delete user payloads
        if (backedUp.empty()) std::filesystem::remove(backup);
    } catch (...) {
        // Restore the old installation; preserve new extracts for diagnostics/retry.
        for (auto it = installed.rbegin(); it != installed.rend(); ++it) std::filesystem::rename(root / *it, stage / *it);
        for (auto it = backedUp.rbegin(); it != backedUp.rend(); ++it) std::filesystem::rename(backup / *it, root / *it);
        [progress close];
        throw;
    }
    [progress close];
    return true;
}
}
int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp finishLaunching];
            [NSApp activate];
            NSBundle* bundle = NSBundle.mainBundle;
            NSString* contents = [bundle.bundlePath stringByAppendingPathComponent:@"Contents"];
            NSString* bin = [contents stringByAppendingPathComponent:@"MacOS"];
            NSString* resources = [contents stringByAppendingPathComponent:@"Resources"];
            const auto support = std::filesystem::path(Text(NSHomeDirectory())) / "Library/Application Support/GT2";
            const char* custom = std::getenv("GT2_DATA_ROOT");
            const auto root = custom && *custom ? std::filesystem::absolute(custom) : support;
            std::filesystem::create_directories(root);
            // Keep one lock across game runs and imports. The spawned game gets
            // a duplicate, so it stays protected even if the launcher is killed.
            const int lock = open((root / ".macos-app.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
                if (lock >= 0) close(lock);
                Fail(@"GT2 is already running or importing discs. Close it before opening another copy.");
            }
            const bool importOnly = argc > 1 && std::string(argv[1]) == "--import";
            if (importOnly || !HasDisc(root)) {
                if (!Import([bin stringByAppendingPathComponent:@"gt2install"], root)) { close(lock); return 0; }
                if (importOnly) { Alert(@"GT2", @"Game data imported. Your saves were kept."); close(lock); return 0; }
            }
            // Pin the bundled driver; system SDKs/Homebrew are not runtime requirements.
            NSString* icd = [resources stringByAppendingPathComponent:@"vulkan/icd.d/MoltenVK_icd.json"];
            setenv("VK_DRIVER_FILES", icd.fileSystemRepresentation, 1);
            setenv("VK_ICD_FILENAMES", icd.fileSystemRepresentation, 1);
            unsetenv("VK_ADD_DRIVER_FILES");
            // SDL resolves vkGetInstanceProcAddr from the linked bundled loader.
            unsetenv("SDL_VULKAN_LIBRARY");
            setenv("GT2_DATA_ROOT", root.c_str(), 1);
            if (!std::getenv("GT2_SAVE_ROOT")) setenv("GT2_SAVE_ROOT", (support / "saves").c_str(), 1);
            std::filesystem::create_directories(support / "logs");
            const auto log = support / "logs/gt2game.log";
            if (!std::freopen(log.c_str(), "a", stdout) || !std::freopen(log.c_str(), "a", stderr)) Fail(@"Cannot open the game log.");
            setenv(gt2mac::kLauncherEnvironment, "1", 1);
            std::vector<std::string> args{Text([bin stringByAppendingPathComponent:@"gt2game"]), "--data-root", root.string(),
                "--save-root", std::getenv("GT2_SAVE_ROOT")};
            for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
            std::vector<char*> pointers;
            for (auto& arg : args) pointers.push_back(arg.data());
            pointers.push_back(nullptr);
            for (;;) {
                posix_spawn_file_actions_t actions;
                int error = posix_spawn_file_actions_init(&actions);
                if (error) throw std::runtime_error(std::strerror(error));
                // A distinct target ensures dup2 clears close-on-exec. The child
                // owns this descriptor until normal exit or process termination.
                error = posix_spawn_file_actions_adddup2(&actions, lock, lock == 32 ? 33 : 32);
                pid_t child = 0;
                if (!error) error = posix_spawn(&child, pointers[0], &actions, nullptr, pointers.data(), *_NSGetEnviron());
                posix_spawn_file_actions_destroy(&actions);
                if (error) throw std::runtime_error(std::string("Cannot start GT2: ") + std::strerror(error));
                [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
                int status = 0;
                for (;;) {
                    const pid_t waited = waitpid(child, &status, WNOHANG);
                    if (waited == child) break;
                    if (waited < 0 && errno != EINTR) throw std::runtime_error(std::strerror(errno));
                    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
                }
                if (!WIFEXITED(status) || WEXITSTATUS(status) != gt2mac::kImportDiscExitCode) {
                    const int result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                    close(lock);
                    return result;
                }
                [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
                [NSApp activate];
                try {
                    Import([bin stringByAppendingPathComponent:@"gt2install"], root);
                } catch (const std::exception& e) {
                    Alert(@"Disc import failed", [NSString stringWithUTF8String:e.what()]);
                }
                // Success, cancel and a recoverable import error all return to the
                // disc picker. The importer keeps previously installed discs/saves.
            }
        } catch (const std::exception& e) {
            Alert(@"GT2 could not start", [NSString stringWithUTF8String:e.what()]);
            return 1;
        }
    }
}
