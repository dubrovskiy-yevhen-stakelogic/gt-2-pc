#include "disc_store.h"
#include "gt2formats/json.h"
#include <gtk/gtk.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace {
class AppLock {
public:
    explicit AppLock(const fs::path& root) {
        // The game inherits this descriptor so a launcher crash cannot unlock it.
        file_ = open((root / ".linux-app.lock").c_str(), O_CREAT | O_RDWR, 0600);
        if (file_ < 0) throw std::runtime_error("Cannot open the application lock.");
        if (flock(file_, LOCK_EX | LOCK_NB) != 0) {
            close(file_); file_ = -1;
            throw std::runtime_error("GT2 is already running. Close it before importing another disc.");
        }
    }
    ~AppLock() { if (file_ >= 0) close(file_); }
    AppLock(const AppLock&) = delete;
    AppLock& operator=(const AppLock&) = delete;
private:
    int file_ = -1;
};

int Run(const std::vector<std::string>& args, std::string* output = nullptr) {
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    int pipes[2] = {-1, -1};
    if (output && pipe(pipes) != 0) throw std::runtime_error("Cannot open tool output pipe.");
    const auto child = fork();
    if (child < 0) {
        if (output) { close(pipes[0]); close(pipes[1]); }
        throw std::runtime_error("Cannot start GT2 tool.");
    }
    if (child == 0) {
        if (output) {
            close(pipes[0]);
            if (dup2(pipes[1], STDOUT_FILENO) < 0) _exit(127);
            close(pipes[1]);
        }
        execv(argv[0], argv.data());
        _exit(127);
    }
    bool readFailed = false;
    if (output) {
        close(pipes[1]);
        char bytes[4096];
        for (;;) {
            const auto count = read(pipes[0], bytes, sizeof(bytes));
            if (count > 0) {
                // Inspection output is tiny. Continue draining unexpected output.
                if (output->size() < 65536) output->append(bytes, size_t(count));
            } else if (count == 0) break;
            else if (errno != EINTR) { readFailed = true; break; }
        }
        close(pipes[0]);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("Cannot wait for GT2 tool.");
    }
    if (readFailed) throw std::runtime_error("Cannot read disc inspection result.");
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}

void Message(const std::string& text) {
    std::fprintf(stderr, "%s\n", text.c_str());
    auto* dialog = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                         GTK_BUTTONS_CLOSE, "%s", text.c_str());
    gtk_window_set_title(GTK_WINDOW(dialog), "GT2");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

std::string ChooseDisc() {
    auto* chooser = gtk_file_chooser_native_new("GT2 - select your raw 2352-byte BIN or ISO",
        nullptr, GTK_FILE_CHOOSER_ACTION_OPEN, "Install disc", "Cancel");
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PlayStation disc image (*.bin, *.iso)");
    for (const auto* pattern : {"*.bin", "*.BIN", "*.iso", "*.ISO"}) gtk_file_filter_add_pattern(filter, pattern);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    std::string selected;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char* name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (name) { selected = name; g_free(name); }
        if (selected.empty()) {
            g_object_unref(chooser);
            throw std::runtime_error("The file picker did not return a local image. Open IMPORT-DISC-STEAMDECK.sh in Desktop Mode.");
        }
    }
    g_object_unref(chooser);
    return selected;
}

void Import(const fs::path& tools, const fs::path& root, const std::string& image) {
    std::string info;
    if (Run({(tools / "gt2install").string(), "inspect", image}, &info) != 0)
        throw std::runtime_error("This disc could not be read. Select a supported raw 2352-byte BIN/ISO. See gt2game.log for details.");
    const auto mode = gt2::json::Parse(info).Require("mode").AsString();
    if (mode != "arcade" && mode != "simulation") throw std::runtime_error("Unsupported disc type.");
    if (fs::exists(fs::symlink_status(root / mode)))
        throw std::runtime_error("This disc is already installed. To add the other disc, select its BIN file.");
    std::string pattern = (root / ".import-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::runtime_error("Cannot create disc staging folder.");
    const fs::path stage(pattern);
    try {
        const auto prepared = stage / "disc";
        if (Run({(tools / "gt2install").string(), "extract", image, prepared.string()}) != 0)
            throw std::runtime_error("Disc installation failed. Existing discs and saves were kept. See gt2game.log for details.");
        gt2linux::PublishDisc(root, mode, prepared);
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(stage, ignored);
        throw;
    }
    std::error_code ignored;
    fs::remove(stage, ignored);
}

void ImportWithProgress(const fs::path& tools, const fs::path& root, const std::string& image) {
    auto* dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "GT2 - installing disc");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 120);
    gtk_window_set_deletable(GTK_WINDOW(dialog), FALSE);
    auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_pack_start(GTK_BOX(area), gtk_label_new("Installing disc. This can take several minutes."), TRUE, TRUE, 16);
    auto* spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(area), spinner, TRUE, TRUE, 16);
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_show_all(dialog);
    std::exception_ptr failure;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        try { Import(tools, root, image); } catch (...) { failure = std::current_exception(); }
        done.store(true);
    });
    while (!done.load()) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        g_usleep(20000);
    }
    worker.join();
    gtk_widget_destroy(dialog);
    if (failure) std::rethrow_exception(failure);
}
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        std::puts("gt2launcher [--import [disc.bin]] [game options]\nAdd discs in Desktop Mode; installed games also run in Gaming Mode.");
        return 0;
    }
    if (!gtk_init_check(nullptr, nullptr)) { std::fputs("GT2 needs a graphical desktop.\n", stderr); return 1; }
    try {
        const auto tools = fs::read_symlink("/proc/self/exe").parent_path();
        const char* custom = std::getenv("GT2_DATA_ROOT");
        const auto root = fs::absolute(custom && *custom ? fs::path(custom) : fs::path(g_get_user_data_dir()) / "GT2");
        fs::create_directories(root);
        AppLock lock(root);
        if (!std::freopen((root / "gt2game.log").c_str(), "a", stdout) || dup2(STDOUT_FILENO, STDERR_FILENO) < 0)
            throw std::runtime_error("Cannot open GT2 log.");
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::printf("GT2 0.7.0 Linux launcher\n");
        setenv("GT2_DATA_ROOT", root.c_str(), 1);
        setenv(gt2linux::kLauncherEnvironment, "1", 1);
        bool importing = !gt2linux::HasDisc(root, "arcade") && !gt2linux::HasDisc(root, "simulation");
        const char* picker = std::getenv("GT2_HOST_DISC_PICKER");
        const bool hostPicker = picker && std::string(picker) == "1";
        std::string image;
        std::vector<std::string> args{(tools / "gt2game").string(), "--data-root", root.string()};
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--import") {
                importing = true;
                if (i + 1 < argc && !std::string(argv[i + 1]).starts_with("--")) image = argv[++i];
            } else if (std::string(argv[i]) == "--data-root") {
                throw std::runtime_error("Use GT2_DATA_ROOT to change the launcher's data folder.");
            } else args.emplace_back(argv[i]);
        }
        for (;;) {
            if (importing) {
                if (image.empty() && hostPicker) return gt2linux::kImportDiscExitCode;
                if (image.empty()) image = ChooseDisc();
                if (image.empty()) return 0;
                try { ImportWithProgress(tools, root, image); }
                catch (const std::exception& e) { Message(e.what()); image.clear(); continue; }
                image.clear();
            }
            const int result = Run(args);
            if (result == gt2linux::kImportDiscExitCode) { importing = true; continue; }
            if (result != 0) Message("GT2 exited with error " + std::to_string(result) + ". Log: " + (root / "gt2game.log").string());
            return result;
        }
    } catch (const std::exception& e) { Message(e.what()); return 1; }
}
