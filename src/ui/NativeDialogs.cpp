#include "NativeDialogs.h"

#include <GLFW/glfw3.h>
#include "Util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#elif !defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace immidi {

NativeDialogs::~NativeDialogs() {
    if (worker_.joinable()) worker_.join();
}

bool NativeDialogs::poll(std::vector<std::string>& paths) {
    if (!finished_) return false;
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lk(mutex_);
    paths = std::move(result_);
    result_.clear();
    finished_ = false;
    running_ = false;
    return true;
}

#if defined(_WIN32) || defined(__APPLE__)

bool NativeDialogs::available() { return true; }

bool NativeDialogs::start(const DialogRequest& req, void* parentWindow) {
    if (running_) return false;
    std::vector<std::string> out;
    // Modal system dialogs must run on the UI thread; they pump the window's messages themselves.
    if (!runNativeDialog(req, parentWindow, out)) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    result_ = std::move(out);
    running_ = true;
    finished_ = true;
    return true;
}

#else

static bool haveProgram(const char* name) {
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return system(cmd.c_str()) == 0;
}

static const char* dialogProgram() {
    static int which = -1;  // 0 none, 1 zenity, 2 kdialog
    if (which < 0) {
        const char* de = getenv("XDG_CURRENT_DESKTOP");
        bool kde = de && (strstr(de, "KDE") || strstr(de, "kde"));
        bool z = haveProgram("zenity"), k = haveProgram("kdialog");
        which = (kde && k) ? 2 : z ? 1 : k ? 2 : 0;
    }
    return which == 1 ? "zenity" : which == 2 ? "kdialog" : nullptr;
}

bool NativeDialogs::available() { return getenv("DISPLAY") || getenv("WAYLAND_DISPLAY") ? dialogProgram() != nullptr : false; }

bool NativeDialogs::start(const DialogRequest& req, void* parentWindow) {
    if (running_ || !available()) return false;
    if (worker_.joinable()) worker_.join();
    running_ = true;
    finished_ = false;
    worker_ = std::thread([this, req, parentWindow] {
        std::vector<std::string> out;
        runNativeDialog(req, parentWindow, out);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            result_ = std::move(out);
            finished_ = true;
        }
        glfwPostEmptyEvent();  // wake the main loop if it is idling
    });
    return true;
}

// Quotes an argument for /bin/sh.
static std::string shq(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
}

static std::string patterns(const DialogFilter& f) {
    std::string p;
    for (const std::string& e : f.extensions) {
        std::string lo = lowerAscii(e), up;
        for (char c : lo) up += char(c >= 'a' && c <= 'z' ? c - 32 : c);
        p += (p.empty() ? "" : " ") + std::string("*.") + lo + " *." + up;
    }
    return p;
}

bool runNativeDialog(const DialogRequest& req, void*, std::vector<std::string>& out) {
    const char* prog = dialogProgram();
    if (!prog) return false;
    std::string dir = req.startDir.empty() ? std::string(getenv("HOME") ? getenv("HOME") : "/") : req.startDir;
    std::string cmd;
    if (std::string(prog) == "zenity") {
        cmd = "zenity --file-selection --title=" + shq(req.title);
        switch (req.kind) {
        case DialogKind::OpenFiles: cmd += " --multiple --separator='\n' --filename=" + shq(dir + "/"); break;
        case DialogKind::OpenFile: cmd += " --filename=" + shq(dir + "/"); break;
        case DialogKind::SelectFolder: cmd += " --directory --filename=" + shq(dir + "/"); break;
        case DialogKind::SaveFile: cmd += " --save --confirm-overwrite --filename=" + shq(dir + "/" + req.defaultName); break;
        }
        if (req.kind != DialogKind::SelectFolder) {
            for (const DialogFilter& f : req.filters) cmd += " --file-filter=" + shq(f.name + " | " + patterns(f));
        }
    } else {
        std::string filter;
        for (const DialogFilter& f : req.filters) filter += (filter.empty() ? "" : "\n") + patterns(f) + "|" + f.name;
        switch (req.kind) {
        case DialogKind::OpenFiles:
            cmd = "kdialog --getopenfilename " + shq(dir) + " " + shq(filter) + " --multiple --separate-output";
            break;
        case DialogKind::OpenFile: cmd = "kdialog --getopenfilename " + shq(dir) + " " + shq(filter); break;
        case DialogKind::SelectFolder: cmd = "kdialog --getexistingdirectory " + shq(dir); break;
        case DialogKind::SaveFile: cmd = "kdialog --getsavefilename " + shq(dir + "/" + req.defaultName) + " " + shq(filter); break;
        }
        cmd += " --title " + shq(req.title);
    }
    cmd += " 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    pclose(f);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return true;
}

#endif

#if defined(_WIN32)

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

static std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string itemPath(IShellItem* item) {
    std::string out;
    PWSTR p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        out = narrow(p);
        CoTaskMemFree(p);
    }
    return out;
}

bool runNativeDialog(const DialogRequest& req, void* parentWindow, std::vector<std::string>& out) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool uninit = SUCCEEDED(init);
    bool save = req.kind == DialogKind::SaveFile;
    IFileDialog* dlg = nullptr;
    HRESULT hr = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(reinterpret_cast<IFileSaveDialog**>(&dlg)))
                      : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(reinterpret_cast<IFileOpenDialog**>(&dlg)));
    if (FAILED(hr) || !dlg) {
        if (uninit) CoUninitialize();
        return false;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM;
    if (req.kind == DialogKind::OpenFiles) opts |= FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST;
    if (req.kind == DialogKind::OpenFile) opts |= FOS_FILEMUSTEXIST;
    if (req.kind == DialogKind::SelectFolder) opts |= FOS_PICKFOLDERS;
    if (save) opts |= FOS_OVERWRITEPROMPT;
    dlg->SetOptions(opts);
    std::wstring title = widen(req.title);
    dlg->SetTitle(title.c_str());

    // Filters (kept alive until Show() returns)
    std::vector<std::wstring> names, specs;
    std::vector<COMDLG_FILTERSPEC> fs;
    if (req.kind != DialogKind::SelectFolder) {
        for (const DialogFilter& f : req.filters) {
            std::string spec;
            for (const std::string& e : f.extensions) spec += (spec.empty() ? "" : ";") + std::string("*.") + e;
            names.push_back(widen(f.name));
            specs.push_back(widen(spec));
        }
        for (size_t i = 0; i < names.size(); i++) fs.push_back({names[i].c_str(), specs[i].c_str()});
        if (!fs.empty()) dlg->SetFileTypes(UINT(fs.size()), fs.data());
        if (save && !req.filters.empty() && !req.filters[0].extensions.empty()) {
            std::wstring ext = widen(req.filters[0].extensions[0]);
            dlg->SetDefaultExtension(ext.c_str());
        }
    }
    if (save && !req.defaultName.empty()) {
        std::wstring name = widen(req.defaultName);
        dlg->SetFileName(name.c_str());
    }
    if (!req.startDir.empty()) {
        std::wstring dir = widen(req.startDir);
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    hr = dlg->Show(static_cast<HWND>(parentWindow));
    if (SUCCEEDED(hr)) {
        if (req.kind == DialogKind::OpenFiles) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(static_cast<IFileOpenDialog*>(dlg)->GetResults(&items)) && items) {
                DWORD count = 0;
                items->GetCount(&count);
                for (DWORD i = 0; i < count; i++) {
                    IShellItem* it = nullptr;
                    if (SUCCEEDED(items->GetItemAt(i, &it)) && it) {
                        std::string p = itemPath(it);
                        if (!p.empty()) out.push_back(p);
                        it->Release();
                    }
                }
                items->Release();
            }
        } else {
            IShellItem* it = nullptr;
            if (SUCCEEDED(dlg->GetResult(&it)) && it) {
                std::string p = itemPath(it);
                if (!p.empty()) out.push_back(p);
                it->Release();
            }
        }
    }
    dlg->Release();
    if (uninit) CoUninitialize();
    return true;  // shown (a cancel simply returns no paths)
}

#endif

} // namespace immidi
