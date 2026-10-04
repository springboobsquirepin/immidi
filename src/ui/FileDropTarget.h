#pragma once
#include <string>
#include <vector>

namespace immidi {

// Windows: GLFW only reports files once they are dropped (WM_DROPFILES). This registers an OLE drop
// target on the window instead, which also reports the position while files are dragged over it.
// Positions are in window client coordinates. Callbacks run on the thread that polls the events.
// Returns false (and leaves GLFW's handling in place) when registration fails or on other systems.
struct FileDropCallbacks {
    void (*hover)(bool inside, double x, double y) = nullptr;
    void (*drop)(const std::vector<std::string>& paths, double x, double y) = nullptr;
};
bool installFileDropTarget(void* nativeWindow, const FileDropCallbacks& callbacks);
void removeFileDropTarget(void* nativeWindow);

} // namespace immidi
