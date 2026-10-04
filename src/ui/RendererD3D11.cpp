// Direct3D 11 renderer (Windows, the default there): a DXGI swap chain on the GLFW window (created
// without an OpenGL context) and Dear ImGui's DirectX 11 backend. Modelled on Dear ImGui's
// example_win32_directx11.
#if defined(_WIN32)

#include "Renderer.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_glfw.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "stb_image_write.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace immidi {

namespace {

template <typename T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Render texture (render target + shader resource).
class D3D11OffscreenTarget final : public OffscreenTarget {
public:
    D3D11OffscreenTarget(ID3D11Device* device, ID3D11DeviceContext* ctx) : device_(device), ctx_(ctx) {}
    ~D3D11OffscreenTarget() override { release(); }

    bool render(ImDrawList* list, int w, int h) override {
        if (!list || w <= 0 || h <= 0 || !ensure(w, h)) return false;
        // Render into the texture, then give the previous render target back.
        ID3D11RenderTargetView* prevRtv = nullptr;
        ID3D11DepthStencilView* prevDsv = nullptr;
        ctx_->OMGetRenderTargets(1, &prevRtv, &prevDsv);
        ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
        const float clear[4] = {0, 0, 0, 0};
        ctx_->ClearRenderTargetView(rtv_, clear);

        ImDrawData dd;
        dd.Valid = true;
        dd.DisplayPos = ImVec2(0, 0);
        dd.DisplaySize = ImVec2(float(w), float(h));
        dd.FramebufferScale = ImVec2(1, 1);
        dd.AddDrawList(list);
        ImGui_ImplDX11_RenderDrawData(&dd);  // sets and restores the viewport itself

        ctx_->OMSetRenderTargets(1, &prevRtv, prevDsv);
        safeRelease(prevRtv);
        safeRelease(prevDsv);
        return true;
    }
    uint64_t texture() const override { return uint64_t(reinterpret_cast<uintptr_t>(srv_)); }
    int width() const override { return w_; }
    int height() const override { return h_; }
    void draw(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1) override { dl->AddImage(ImTextureRef(ImTextureID(texture())), p0, p1); }

private:
    bool ensure(int w, int h) {
        if (failed_) return false;
        if (tex_ && w == w_ && h == h_) return true;
        release();
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = UINT(w);
        desc.Height = UINT(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &tex_)) || FAILED(device_->CreateRenderTargetView(tex_, nullptr, &rtv_)) ||
            FAILED(device_->CreateShaderResourceView(tex_, nullptr, &srv_))) {
            release();
            failed_ = true;
            return false;
        }
        w_ = w;
        h_ = h;
        return true;
    }
    void release() {
        safeRelease(srv_);
        safeRelease(rtv_);
        safeRelease(tex_);
        w_ = h_ = 0;
    }
    ID3D11Device* device_;
    ID3D11DeviceContext* ctx_;
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    int w_ = 0, h_ = 0;
    bool failed_ = false;
};

class D3D11Renderer final : public Renderer {
public:
    RendererKind kind() const override { return RendererKind::D3D11; }
    void windowHints() override { glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); }

    bool init(GLFWwindow* window, std::string* error) override {
        window_ = window;
        HWND hwnd = glfwGetWin32Window(window);
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        // Flip model first (Windows 10: better vsync and presentation), the classic model on older
        // systems; hardware first, then the WARP software rasterizer.
        const DXGI_SWAP_EFFECT effects[2] = {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_DISCARD};
        const D3D_DRIVER_TYPE drivers[2] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
        const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = E_FAIL;
        for (D3D_DRIVER_TYPE driver : drivers) {
            for (DXGI_SWAP_EFFECT effect : effects) {
                DXGI_SWAP_CHAIN_DESC sd = {};
                sd.BufferCount = 2;
                sd.BufferDesc.Width = UINT(w > 0 ? w : 1);
                sd.BufferDesc.Height = UINT(h > 0 ? h : 1);
                sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.OutputWindow = hwnd;
                sd.SampleDesc.Count = 1;
                sd.Windowed = TRUE;
                sd.SwapEffect = effect;
                D3D_FEATURE_LEVEL got;
                hr = D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &got,
                                                   &ctx_);
                if (SUCCEEDED(hr)) break;
            }
            if (SUCCEEDED(hr)) break;
        }
        if (FAILED(hr)) {
            if (error) *error = "Direct3D 11 is not available (HRESULT 0x" + hex(uint32_t(hr)) + ")";
            return false;
        }
        // Queue at most one frame, so the CPU does not run ahead of the GPU.
        IDXGIDevice1* dxgiDevice = nullptr;
        if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgiDevice)))) {
            dxgiDevice->SetMaximumFrameLatency(1);
            dxgiDevice->Release();
        }
        // ALT+ENTER fullscreen is not wanted.
        IDXGIFactory* factory = nullptr;
        if (SUCCEEDED(swapChain_->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
            factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            factory->Release();
        }
        return createRenderTarget();
    }

    bool initImGui() override { return ImGui_ImplGlfw_InitForOther(window_, true) && ImGui_ImplDX11_Init(device_, ctx_); }
    void shutdownImGui() override {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }
    void shutdown() override {
        releaseDeleted();
        safeRelease(rtv_);
        safeRelease(swapChain_);
        if (ctx_) ctx_->ClearState();
        safeRelease(ctx_);
        safeRelease(device_);
    }
    void newFrame() override { ImGui_ImplDX11_NewFrame(); }

    void render(ImDrawData* data, const float c[4]) override {
        // A minimized or hidden window does not need drawing (Present would return at once).
        if (occluded_ && swapChain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            skipPresent_ = true;
            Sleep(10);
            return;
        }
        occluded_ = false;
        skipPresent_ = false;
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        if (w > 0 && h > 0 && (w != bufW_ || h != bufH_)) {
            // The buffers may not be referenced (not even bound) while they are resized.
            ctx_->OMSetRenderTargets(0, nullptr, nullptr);
            safeRelease(rtv_);
            swapChain_->ResizeBuffers(0, UINT(w), UINT(h), DXGI_FORMAT_UNKNOWN, 0);
            createRenderTarget();
        }
        if (!rtv_) return;
        ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
        ctx_->ClearRenderTargetView(rtv_, c);
        ImGui_ImplDX11_RenderDrawData(data);
    }

    void present() override {
        releaseDeleted();
        if (skipPresent_ || !swapChain_) return;
        HRESULT hr = swapChain_->Present(1, 0);  // vsync
        occluded_ = hr == DXGI_STATUS_OCCLUDED;
    }

    bool screenshot(const std::string& path) override {
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
        D3D11_TEXTURE2D_DESC desc;
        back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        bool ok = false;
        if (SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &staging))) {
            ctx_->CopyResource(staging, back);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                std::vector<unsigned char> px(size_t(desc.Width) * desc.Height * 4);
                for (UINT y = 0; y < desc.Height; y++)
                    std::memcpy(&px[size_t(y) * desc.Width * 4], static_cast<const unsigned char*>(m.pData) + size_t(y) * m.RowPitch, size_t(desc.Width) * 4);
                ctx_->Unmap(staging, 0);
                ok = stbi_write_png(path.c_str(), int(desc.Width), int(desc.Height), 4, px.data(), int(desc.Width) * 4) != 0;
            }
            staging->Release();
        }
        back->Release();
        return ok;
    }

    std::unique_ptr<OffscreenTarget> createOffscreenTarget() override { return std::make_unique<D3D11OffscreenTarget>(device_, ctx_); }

    uint64_t uploadPixelTexture(uint64_t texture, int w, int h, const uint32_t* rgba) override {
        deletePixelTexture(texture);
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = UINT(w);
        desc.Height = UINT(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // IM_COL32 byte order
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {rgba, UINT(w) * 4, 0};
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        if (FAILED(device_->CreateTexture2D(&desc, &init, &tex))) return 0;
        HRESULT hr = device_->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();  // the view keeps the texture alive
        return SUCCEEDED(hr) ? uint64_t(reinterpret_cast<uintptr_t>(srv)) : 0;
    }
    // Released after the frame was drawn: the frame's draw lists may still refer to the view (the
    // GPU side is kept alive by Direct3D for as long as queued commands use it).
    void deletePixelTexture(uint64_t texture) override {
        if (texture) deleted_.push_back(reinterpret_cast<ID3D11ShaderResourceView*>(uintptr_t(texture)));
    }

private:
    static std::string hex(uint32_t v) {
        char b[16];
        snprintf(b, sizeof b, "%08X", v);
        return b;
    }
    void releaseDeleted() {
        for (ID3D11ShaderResourceView* v : deleted_) v->Release();
        deleted_.clear();
    }
    bool createRenderTarget() {
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
        D3D11_TEXTURE2D_DESC desc;
        back->GetDesc(&desc);
        bufW_ = int(desc.Width);
        bufH_ = int(desc.Height);
        HRESULT hr = device_->CreateRenderTargetView(back, nullptr, &rtv_);
        back->Release();
        return SUCCEEDED(hr);
    }
    GLFWwindow* window_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    std::vector<ID3D11ShaderResourceView*> deleted_;
    int bufW_ = 0, bufH_ = 0;
    bool occluded_ = false, skipPresent_ = false;
};

} // namespace

std::unique_ptr<Renderer> createD3D11Renderer() { return std::make_unique<D3D11Renderer>(); }

} // namespace immidi

#endif  // _WIN32
