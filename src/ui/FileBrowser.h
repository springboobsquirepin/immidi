#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace immidi {

// Minimal cross-platform file dialog drawn with Dear ImGui.
class FileBrowser {
public:
    enum class Mode { OpenFiles, SelectFolder, SaveFile };

    void open(Mode mode, const std::string& title, const std::string& startDir, const std::vector<std::string>& extensions,
              const std::string& defaultName = {});
    // Draws the dialog while open. Returns true on the frame the user confirmed.
    bool draw();
    bool isOpen() const { return open_; }
    const std::vector<std::string>& selection() const { return selection_; }
    const std::string& currentDir() const { return dir_; }
    bool recursive() const { return recursive_; }

private:
    struct Entry {
        std::string name;
        std::string path;
        bool isDir;
        uintmax_t size;
    };
    void navigate(const std::string& dir);
    bool matches(const std::string& name) const;

    Mode mode_ = Mode::OpenFiles;
    std::string title_;
    std::string dir_;
    std::vector<std::string> exts_;
    std::vector<Entry> entries_;
    std::vector<bool> selected_;
    std::vector<std::string> selection_;
    char pathBuf_[1024] = {};
    char nameBuf_[256] = {};
    char filterBuf_[64] = {};
    int lastClicked_ = -1;
    bool open_ = false;
    bool justOpened_ = false;
    bool showAll_ = false;
    bool recursive_ = true;
    std::string error_;
};

} // namespace immidi
