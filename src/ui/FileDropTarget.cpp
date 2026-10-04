#include "FileDropTarget.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>

namespace immidi {

namespace {

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? size_t(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

class DropTarget final : public IDropTarget {
public:
    DropTarget(HWND hwnd, const FileDropCallbacks& cb) : hwnd_(hwnd), cb_(cb) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return ULONG(r);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        accept_ = data && data->QueryGetData(&fmt) == S_OK;
        return DragOver(0, pt, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (accept_ && cb_.hover) {
            POINT p = clientPoint(pt);
            cb_.hover(true, p.x, p.y);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (cb_.hover) cb_.hover(false, 0, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        if (cb_.hover) cb_.hover(false, 0, 0);
        *effect = DROPEFFECT_NONE;
        FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM med = {};
        if (!accept_ || !data || FAILED(data->GetData(&fmt, &med))) return S_OK;
        std::vector<std::string> paths;
        if (HDROP drop = static_cast<HDROP>(GlobalLock(med.hGlobal))) {
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; i++) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring w(len, L'\0');
                DragQueryFileW(drop, i, &w[0], len + 1);
                paths.push_back(narrow(w.c_str()));
            }
            GlobalUnlock(med.hGlobal);
        }
        ReleaseStgMedium(&med);
        if (!paths.empty() && cb_.drop) {
            POINT p = clientPoint(pt);
            cb_.drop(paths, p.x, p.y);
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }

private:
    POINT clientPoint(POINTL pt) const {
        POINT p = {pt.x, pt.y};
        ScreenToClient(hwnd_, &p);
        return p;
    }
    LONG refs_ = 1;
    HWND hwnd_;
    FileDropCallbacks cb_;
    bool accept_ = false;
};

} // namespace

bool installFileDropTarget(void* nativeWindow, const FileDropCallbacks& callbacks) {
    HWND hwnd = static_cast<HWND>(nativeWindow);
    if (!hwnd) return false;
    HRESULT init = OleInitialize(nullptr);  // S_FALSE when already initialized on this thread
    if (FAILED(init)) return false;
    auto* target = new DropTarget(hwnd, callbacks);
    DragAcceptFiles(hwnd, FALSE);  // the OLE target replaces GLFW's WM_DROPFILES handling
    HRESULT hr = RegisterDragDrop(hwnd, target);
    target->Release();  // RegisterDragDrop keeps its own reference
    if (FAILED(hr)) {
        DragAcceptFiles(hwnd, TRUE);
        return false;
    }
    return true;
}

void removeFileDropTarget(void* nativeWindow) {
    if (nativeWindow) RevokeDragDrop(static_cast<HWND>(nativeWindow));
}

} // namespace immidi

#else

namespace immidi {
bool installFileDropTarget(void*, const FileDropCallbacks&) { return false; }
void removeFileDropTarget(void*) {}
} // namespace immidi

#endif
