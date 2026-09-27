#pragma once

// The launcher keeps the data-directory lock while the child shuts down before
// importing. No importer runs while a game still has the disc files open.
namespace gt2mac {
inline constexpr int kImportDiscExitCode = 42;
inline constexpr const char* kLauncherEnvironment = "GT2_MAC_LAUNCHER";
}
