#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "imgui.h"

struct GLFWwindow;

namespace immidi {

// The graphics API the window draws with. Each platform defaults to its native API: Direct3D 11
// on Windows, Metal on macOS and Vulkan on Linux. OpenGL is deprecated: it is only used when the
// native API cannot start, or when it is chosen explicitly.
enum class RendererKind { OpenGL, D3D11, Metal, Vulkan };
const char* rendererName(RendererKind kind);  // "Direct3D 11", "Metal", "Vulkan", "OpenGL"
const char* rendererKey(RendererKind kind);   // the setting / --renderer value: "d3d11", "metal", "vulkan", "opengl"
bool rendererAvailable(RendererKind kind);    // built into this platform's version
RendererKind defaultRenderer();
bool parseRendererName(const std::string& name, RendererKind* out);

// A texture that ImGui draw lists are rendered into (the Sound Canvas display), shown scaled down
// with bilinear filtering. It is rendered at most twice the size it is shown at, so every screen
// pixel blends the texels it covers and the dot matrix stays even.
class OffscreenTarget {
public:
    virtual ~OffscreenTarget() = default;
    // Renders `list` (coordinates in pixels, origin top left) into a w x h texture. Must run after
    // the frame's ImGui draw data was rendered, so glyphs added this frame are already in the font
    // texture.
    virtual bool render(ImDrawList* list, int w, int h) = 0;
    virtual uint64_t texture() const = 0;  // ImTextureID, 0 until rendered
    virtual int width() const = 0;
    virtual int height() const = 0;
    // Adds the texture to `dl`, scaled into p0..p1 (bilinear).
    virtual void draw(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1) = 0;
};

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual RendererKind kind() const = 0;
    // Window hints for glfwCreateWindow (an OpenGL context, or no client API for the others).
    virtual void windowHints() = 0;
    // Creates the device / context for the window. False (with a reason) when it is unavailable.
    virtual bool init(GLFWwindow* window, std::string* error) = 0;
    // Dear ImGui platform + renderer backends (the ImGui context must exist).
    virtual bool initImGui() = 0;
    virtual void shutdownImGui() = 0;
    virtual void shutdown() = 0;  // before the window is destroyed
    virtual void newFrame() = 0;
    // Clears the window and draws the frame (resizes the back buffer when the window changed).
    virtual void render(ImDrawData* data, const float clearColor[4]) = 0;
    virtual void present() = 0;
    // Saves the frame drawn by render() (call it before present()).
    virtual bool screenshot(const std::string& pngPath) = 0;
    virtual std::unique_ptr<OffscreenTarget> createOffscreenTarget() = 0;
    // RGBA pixels (IM_COL32 layout, top row first) as a texture drawn 1:1; replaces `texture`.
    // Deleting is safe while the current frame still refers to the texture: the renderers free it
    // once the GPU is done with it.
    virtual uint64_t uploadPixelTexture(uint64_t texture, int w, int h, const uint32_t* rgba) = 0;
    virtual void deletePixelTexture(uint64_t texture) = 0;
};

std::unique_ptr<Renderer> createRenderer(RendererKind kind);

// The renderer of the window, for the widgets and views that create textures.
Renderer* activeRenderer();
void setActiveRenderer(Renderer* renderer);
uint64_t uploadPixelTexture(uint64_t texture, int w, int h, const uint32_t* rgba);
void deletePixelTexture(uint64_t texture);

} // namespace immidi
