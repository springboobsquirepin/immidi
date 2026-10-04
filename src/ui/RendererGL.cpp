// OpenGL renderer (all platforms, deprecated): GLFW's OpenGL context with Dear ImGui's OpenGL 3
// backend. Only used when the platform's native API (Direct3D 11, Metal, Vulkan) cannot start, or
// when chosen in the settings / with --renderer opengl.
#include "Renderer.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(__APPLE__) && !defined(GL_SILENCE_DEPRECATION)
#define GL_SILENCE_DEPRECATION  // OpenGL still works on macOS; Apple only marks it deprecated
#endif
#include <GLFW/glfw3.h>

#include "stb_image_write.h"

#include <cstring>
#include <vector>

#ifndef APIENTRY
#define APIENTRY
#endif

namespace immidi {

namespace {

// GL 3.0 framebuffer objects are not in every platform's GL headers (Windows ships GL 1.1), so the
// functions are looked up at run time. Texture calls are GL 1.1.
constexpr GLenum kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0, kFramebufferComplete = 0x8CD5,
                 kFramebufferBinding = 0x8CA6, kClampToEdge = 0x812F, kTextureMaxLevel = 0x813D;
typedef void(APIENTRY* PfnGenFramebuffers)(GLsizei, GLuint*);
typedef void(APIENTRY* PfnDeleteFramebuffers)(GLsizei, const GLuint*);
typedef void(APIENTRY* PfnBindFramebuffer)(GLenum, GLuint);
typedef void(APIENTRY* PfnFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY* PfnCheckFramebufferStatus)(GLenum);

struct Fbo {
    PfnGenFramebuffers gen = nullptr;
    PfnDeleteFramebuffers del = nullptr;
    PfnBindFramebuffer bind = nullptr;
    PfnFramebufferTexture2D tex2d = nullptr;
    PfnCheckFramebufferStatus check = nullptr;
    bool ok() const { return gen && del && bind && tex2d && check; }
};

const Fbo& fbo() {
    static Fbo f = [] {
        Fbo r;
        r.gen = reinterpret_cast<PfnGenFramebuffers>(glfwGetProcAddress("glGenFramebuffers"));
        r.del = reinterpret_cast<PfnDeleteFramebuffers>(glfwGetProcAddress("glDeleteFramebuffers"));
        r.bind = reinterpret_cast<PfnBindFramebuffer>(glfwGetProcAddress("glBindFramebuffer"));
        r.tex2d = reinterpret_cast<PfnFramebufferTexture2D>(glfwGetProcAddress("glFramebufferTexture2D"));
        r.check = reinterpret_cast<PfnCheckFramebufferStatus>(glfwGetProcAddress("glCheckFramebufferStatus"));
        return r;
    }();
    return f;
}

// Framebuffer object + texture.
class GLOffscreenTarget final : public OffscreenTarget {
public:
    ~GLOffscreenTarget() override {
        if (fbo_ && fbo().ok()) fbo().del(1, &fbo_);
        if (tex_) glDeleteTextures(1, &tex_);
    }

    bool render(ImDrawList* list, int w, int h) override {
        if (!list || w <= 0 || h <= 0 || !ensure(w, h)) return false;
        GLint lastFbo = 0, lastViewport[4] = {0, 0, 0, 0};
        glGetIntegerv(kFramebufferBinding, &lastFbo);
        glGetIntegerv(GL_VIEWPORT, lastViewport);
        fbo().bind(kFramebuffer, fbo_);
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        ImDrawData dd;
        dd.Valid = true;
        dd.DisplayPos = ImVec2(0, 0);
        dd.DisplaySize = ImVec2(float(w), float(h));
        dd.FramebufferScale = ImVec2(1, 1);
        dd.AddDrawList(list);
        ImGui_ImplOpenGL3_RenderDrawData(&dd);

        fbo().bind(kFramebuffer, GLuint(lastFbo));
        glViewport(lastViewport[0], lastViewport[1], lastViewport[2], lastViewport[3]);
        return true;
    }
    uint64_t texture() const override { return w_ ? tex_ : 0; }
    int width() const override { return w_; }
    int height() const override { return h_; }
    void draw(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1) override {
        dl->AddImage(ImTextureRef(ImTextureID(tex_)), p0, p1, ImVec2(0, 1), ImVec2(1, 0));  // render targets are bottom-up
    }

private:
    bool ensure(int w, int h) {
        if (failed_ || !fbo().ok()) return false;
        if (tex_ && w == w_ && h == h_) return true;
        if (!tex_) glGenTextures(1, &tex_);
        if (!fbo_) fbo().gen(1, &fbo_);
        GLint lastTex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTex);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // as Dear ImGui's sampler on GL 3.3+
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, kTextureMaxLevel, 0);
        GLint lastFbo = 0;
        glGetIntegerv(kFramebufferBinding, &lastFbo);
        fbo().bind(kFramebuffer, fbo_);
        fbo().tex2d(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, tex_, 0);
        bool complete = fbo().check(kFramebuffer) == kFramebufferComplete;
        fbo().bind(kFramebuffer, GLuint(lastFbo));
        glBindTexture(GL_TEXTURE_2D, GLuint(lastTex));
        if (!complete) {
            failed_ = true;
            return false;
        }
        w_ = w;
        h_ = h;
        return true;
    }
    GLuint fbo_ = 0, tex_ = 0;
    int w_ = 0, h_ = 0;
    bool failed_ = false;
};

class GLRenderer final : public Renderer {
public:
    RendererKind kind() const override { return RendererKind::OpenGL; }

    void windowHints() override {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
#if defined(__APPLE__)
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    }

    bool init(GLFWwindow* window, std::string*) override {
        window_ = window;
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);
        return true;
    }

    bool initImGui() override {
#if defined(__APPLE__)
        const char* glsl = "#version 150";
#else
        const char* glsl = "#version 130";
#endif
        return ImGui_ImplGlfw_InitForOpenGL(window_, true) && ImGui_ImplOpenGL3_Init(glsl);
    }
    void shutdownImGui() override {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }
    void shutdown() override { freeTextures(); }
    void newFrame() override { ImGui_ImplOpenGL3_NewFrame(); }

    void render(ImDrawData* data, const float c[4]) override {
        int w, h;
        glfwGetFramebufferSize(window_, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(c[0], c[1], c[2], c[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(data);
    }
    void present() override {
        glfwSwapBuffers(window_);
        freeTextures();
    }

    bool screenshot(const std::string& path) override {
        int w, h;
        glfwGetFramebufferSize(window_, &w, &h);
        std::vector<unsigned char> px(size_t(w) * size_t(h) * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        std::vector<unsigned char> flipped(px.size());  // OpenGL rows are bottom-up
        for (int y = 0; y < h; y++) std::memcpy(&flipped[size_t(y) * w * 4], &px[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
        return stbi_write_png(path.c_str(), w, h, 4, flipped.data(), w * 4) != 0;
    }

    std::unique_ptr<OffscreenTarget> createOffscreenTarget() override { return std::make_unique<GLOffscreenTarget>(); }

    uint64_t uploadPixelTexture(uint64_t texture, int w, int h, const uint32_t* rgba) override {
        GLuint tex = GLuint(texture);
        if (!tex) glGenTextures(1, &tex);
        GLint lastTex = 0, lastAlign = 4;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTex);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &lastAlign);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glPixelStorei(GL_UNPACK_ALIGNMENT, lastAlign);
        glBindTexture(GL_TEXTURE_2D, GLuint(lastTex));
        return tex;
    }
    // Deleted after the frame was drawn (the frame's draw lists may still refer to the texture).
    void deletePixelTexture(uint64_t texture) override {
        if (texture) deleted_.push_back(GLuint(texture));
    }

private:
    void freeTextures() {
        if (!deleted_.empty()) glDeleteTextures(GLsizei(deleted_.size()), deleted_.data());
        deleted_.clear();
    }
    GLFWwindow* window_ = nullptr;
    std::vector<GLuint> deleted_;
};

} // namespace

std::unique_ptr<Renderer> createOpenGLRenderer() { return std::make_unique<GLRenderer>(); }

} // namespace immidi
