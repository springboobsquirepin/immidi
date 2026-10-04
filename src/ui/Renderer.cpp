// Graphics API selection, shared by the renderers.
#include "Renderer.h"

namespace immidi {

#if defined(_WIN32)
std::unique_ptr<Renderer> createD3D11Renderer();   // RendererD3D11.cpp
#endif
#if defined(__APPLE__)
std::unique_ptr<Renderer> createMetalRenderer();   // RendererMetal.mm
#endif
#if defined(IMMIDI_VULKAN)
std::unique_ptr<Renderer> createVulkanRenderer();  // RendererVulkan.cpp
#endif
std::unique_ptr<Renderer> createOpenGLRenderer();  // RendererGL.cpp

namespace {
Renderer* g_active = nullptr;
}

const char* rendererName(RendererKind kind) {
    switch (kind) {
    case RendererKind::D3D11: return "Direct3D 11";
    case RendererKind::Metal: return "Metal";
    case RendererKind::Vulkan: return "Vulkan";
    case RendererKind::OpenGL: break;
    }
    return "OpenGL";
}

const char* rendererKey(RendererKind kind) {
    switch (kind) {
    case RendererKind::D3D11: return "d3d11";
    case RendererKind::Metal: return "metal";
    case RendererKind::Vulkan: return "vulkan";
    case RendererKind::OpenGL: break;
    }
    return "opengl";
}

bool rendererAvailable(RendererKind kind) {
    switch (kind) {
    case RendererKind::OpenGL: return true;
    case RendererKind::D3D11:
#if defined(_WIN32)
        return true;
#else
        return false;
#endif
    case RendererKind::Metal:
#if defined(__APPLE__)
        return true;
#else
        return false;
#endif
    case RendererKind::Vulkan:
#if defined(IMMIDI_VULKAN)
        return true;
#else
        return false;
#endif
    }
    return false;
}

RendererKind defaultRenderer() {
#if defined(_WIN32)
    return RendererKind::D3D11;
#elif defined(__APPLE__)
    return RendererKind::Metal;
#elif defined(IMMIDI_VULKAN)
    return RendererKind::Vulkan;
#else
    return RendererKind::OpenGL;
#endif
}

bool parseRendererName(const std::string& name, RendererKind* out) {
    if (name == "opengl" || name == "gl") *out = RendererKind::OpenGL;
    else if (name == "d3d11" || name == "dx11" || name == "direct3d11") *out = RendererKind::D3D11;
    else if (name == "metal" || name == "mtl") *out = RendererKind::Metal;
    else if (name == "vulkan" || name == "vk") *out = RendererKind::Vulkan;
    else return false;
    return true;
}

std::unique_ptr<Renderer> createRenderer(RendererKind kind) {
#if defined(_WIN32)
    if (kind == RendererKind::D3D11) return createD3D11Renderer();
#endif
#if defined(__APPLE__)
    if (kind == RendererKind::Metal) return createMetalRenderer();
#endif
#if defined(IMMIDI_VULKAN)
    if (kind == RendererKind::Vulkan) return createVulkanRenderer();
#endif
    if (kind == RendererKind::OpenGL) return createOpenGLRenderer();
    return nullptr;
}

Renderer* activeRenderer() { return g_active; }
void setActiveRenderer(Renderer* r) { g_active = r; }

uint64_t uploadPixelTexture(uint64_t texture, int w, int h, const uint32_t* rgba) {
    return g_active ? g_active->uploadPixelTexture(texture, w, h, rgba) : 0;
}

void deletePixelTexture(uint64_t texture) {
    if (g_active && texture) g_active->deletePixelTexture(texture);
}

} // namespace immidi
