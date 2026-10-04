#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace immidi {

// Operating system file dialogs:
//   Windows: IFileOpenDialog / IFileSaveDialog (modal, run on the UI thread)
//   macOS:   NSOpenPanel / NSSavePanel (modal, run on the UI thread)
//   Linux:   zenity or kdialog, run on a worker thread so the window keeps drawing
struct DialogFilter {
    std::string name;
    std::vector<std::string> extensions;  // without the dot, e.g. "mid"
};

enum class DialogKind { OpenFiles, OpenFile, SelectFolder, SaveFile };

struct DialogRequest {
    DialogKind kind = DialogKind::OpenFiles;
    std::string title;
    std::string startDir;
    std::string defaultName;  // SaveFile
    std::vector<DialogFilter> filters;
};

class NativeDialogs {
public:
    ~NativeDialogs();
    // True when a native dialog can be shown on this system.
    static bool available();
    // Starts a dialog. `parentWindow` is the native window handle (HWND / NSWindow*), may be null.
    // Returns false when no native dialog could be started (use the built-in browser then).
    bool start(const DialogRequest& req, void* parentWindow);
    bool busy() const { return running_; }
    // Returns true once when the dialog finished; `paths` is empty when cancelled.
    bool poll(std::vector<std::string>& paths);

private:
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::mutex mutex_;
    std::vector<std::string> result_;
};

// Platform implementation (synchronous). Returns false if the dialog could not be shown at all.
bool runNativeDialog(const DialogRequest& req, void* parentWindow, std::vector<std::string>& out);

} // namespace immidi
