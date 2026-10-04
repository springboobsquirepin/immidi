// Metal renderer (macOS, the default there): a CAMetalLayer on the GLFW window's content view and
// Dear ImGui's Metal backend. Compiled with ARC (-fobjc-arc).
#if defined(__APPLE__)

#include "Renderer.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "stb_image_write.h"

#include <cstring>
#include <vector>

#if !__has_feature(objc_arc)
#error "RendererMetal.mm is compiled with -fobjc-arc"
#endif

namespace immidi {

namespace {

// Dear ImGui's Metal backend takes an id<MTLTexture> as ImTextureID.
uint64_t toId(id<MTLTexture> texture) { return uint64_t(uintptr_t((__bridge void*)texture)); }

class MetalRenderer;

// Render texture with one level, which Dear ImGui's linear sampler draws bilinear.
class MetalOffscreenTarget final : public OffscreenTarget {
public:
    explicit MetalOffscreenTarget(MetalRenderer* r) : r_(r) {}
    bool render(ImDrawList* list, int w, int h) override;
    uint64_t texture() const override { return toId(tex_); }
    int width() const override { return w_; }
    int height() const override { return h_; }
    void draw(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1) override { dl->AddImage(ImTextureRef(ImTextureID(texture())), p0, p1); }

private:
    MetalRenderer* r_;
    id<MTLTexture> tex_ = nil;
    int w_ = 0, h_ = 0;
};

class MetalRenderer final : public Renderer {
public:
    RendererKind kind() const override { return RendererKind::Metal; }
    void windowHints() override { glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); }

    bool init(GLFWwindow* window, std::string* error) override {
        @autoreleasepool {
            window_ = window;
            device_ = MTLCreateSystemDefaultDevice();
            if (!device_) {
                if (error) *error = "no Metal device";
                return false;
            }
            queue_ = [device_ newCommandQueue];
            NSWindow* nswin = glfwGetCocoaWindow(window);
            if (!queue_ || !nswin) {
                if (error) *error = "the Metal command queue or the window is not available";
                return false;
            }
            layer_ = [CAMetalLayer layer];
            layer_.device = device_;
            layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer_.contentsScale = nswin.backingScaleFactor;
            nswin.contentView.layer = layer_;
            nswin.contentView.wantsLayer = YES;
            // Dear ImGui's NewFrame reads the frame's pixel format from a render pass descriptor: a
            // 1 x 1 texture of that format stands in for the drawable, which is only requested when
            // the frame is drawn.
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            formatPass_ = [MTLRenderPassDescriptor renderPassDescriptor];
            formatPass_.colorAttachments[0].texture = [device_ newTextureWithDescriptor:td];
            return formatPass_.colorAttachments[0].texture != nil;
        }
    }

    bool initImGui() override {
        @autoreleasepool {
            return ImGui_ImplGlfw_InitForOther(window_, true) && ImGui_ImplMetal_Init(device_);
        }
    }
    void shutdownImGui() override {
        @autoreleasepool {
            ImGui_ImplMetal_Shutdown();
            ImGui_ImplGlfw_Shutdown();
        }
    }
    void shutdown() override {
        @autoreleasepool {
            if (queue_) {  // let the GPU finish
                id<MTLCommandBuffer> cb = [queue_ commandBuffer];
                [cb commit];
                [cb waitUntilCompleted];
            }
            deleted_.clear();
            cmd_ = nil;
            drawable_ = nil;
            formatPass_ = nil;
            layer_ = nil;
            queue_ = nil;
            device_ = nil;
        }
    }

    void newFrame() override {
        @autoreleasepool {
            ImGui_ImplMetal_NewFrame(formatPass_);
        }
    }

    void render(ImDrawData* data, const float c[4]) override {
        @autoreleasepool {
            cmd_ = nil;
            drawable_ = nil;
            int w = 0, h = 0;
            glfwGetFramebufferSize(window_, &w, &h);
            if (w <= 0 || h <= 0) return;
            NSWindow* nswin = glfwGetCocoaWindow(window_);
            if (nswin && layer_.contentsScale != nswin.backingScaleFactor) layer_.contentsScale = nswin.backingScaleFactor;
            const CGSize size = CGSizeMake(w, h);
            if (!CGSizeEqualToSize(layer_.drawableSize, size)) layer_.drawableSize = size;
            drawable_ = [layer_ nextDrawable];  // waits for a free drawable (vsync)
            if (!drawable_) return;
            cmd_ = [queue_ commandBuffer];
            for (int i = 0; i < 4; i++) clear_[i] = c[i];
            MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = drawable_.texture;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            rpd.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
            id<MTLRenderCommandEncoder> enc = [cmd_ renderCommandEncoderWithDescriptor:rpd];
            ImGui_ImplMetal_RenderDrawData(data, cmd_, enc);
            [enc endEncoding];
        }
    }

    void present() override {
        @autoreleasepool {
            if (cmd_) {
                if (drawable_) [cmd_ presentDrawable:drawable_];
                [cmd_ commit];
            }
            cmd_ = nil;
            drawable_ = nil;
            deleted_.clear();  // command buffers keep the textures they use alive
        }
    }

    // Draws the frame again into a readable texture (the drawable itself is framebuffer-only).
    bool screenshot(const std::string& path) override {
        @autoreleasepool {
            ImDrawData* data = ImGui::GetDrawData();
            if (!cmd_ || !data) return false;
            const NSUInteger w = NSUInteger(layer_.drawableSize.width), h = NSUInteger(layer_.drawableSize.height);
            if (!w || !h) return false;
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> target = [device_ newTextureWithDescriptor:td];
            id<MTLBuffer> buffer = [device_ newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
            if (!target || !buffer) return false;
            id<MTLCommandBuffer> cb = [queue_ commandBuffer];
            MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = target;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            rpd.colorAttachments[0].clearColor = MTLClearColorMake(clear_[0], clear_[1], clear_[2], clear_[3]);
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
            ImGui_ImplMetal_RenderDrawData(data, cb, enc);
            [enc endEncoding];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:target
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(w, h, 1)
                                toBuffer:buffer
                       destinationOffset:0
                  destinationBytesPerRow:w * 4
                destinationBytesPerImage:w * h * 4];
            [blit endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            std::vector<unsigned char> px(w * h * 4);
            std::memcpy(px.data(), buffer.contents, px.size());
            for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);  // BGRA -> RGBA
            return stbi_write_png(path.c_str(), int(w), int(h), 4, px.data(), int(w) * 4) != 0;
        }
    }

    std::unique_ptr<OffscreenTarget> createOffscreenTarget() override { return std::make_unique<MetalOffscreenTarget>(this); }

    uint64_t uploadPixelTexture(uint64_t texture, int w, int h, const uint32_t* rgba) override {
        @autoreleasepool {
            deletePixelTexture(texture);
            if (w <= 0 || h <= 0) return 0;
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:NSUInteger(w)
                                                                                        height:NSUInteger(h)
                                                                                     mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> tex = [device_ newTextureWithDescriptor:td];
            if (!tex) return 0;
            [tex replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(w), NSUInteger(h)) mipmapLevel:0 withBytes:rgba bytesPerRow:NSUInteger(w) * 4];
            return uint64_t(uintptr_t((__bridge_retained void*)tex));  // released by deletePixelTexture
        }
    }
    // Kept until the frame was committed: the frame's draw lists may still refer to the texture.
    void deletePixelTexture(uint64_t texture) override {
        if (texture) deleted_.push_back((__bridge_transfer id<MTLTexture>)(void*)uintptr_t(texture));
    }

    id<MTLDevice> device() const { return device_; }
    id<MTLCommandBuffer> frameCommands() const { return cmd_; }  // nil when the frame is not drawn

private:
    GLFWwindow* window_ = nullptr;
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    CAMetalLayer* layer_ = nil;
    MTLRenderPassDescriptor* formatPass_ = nil;
    id<MTLCommandBuffer> cmd_ = nil;
    id<CAMetalDrawable> drawable_ = nil;
    std::vector<id<MTLTexture>> deleted_;
    float clear_[4] = {0, 0, 0, 1};
};

bool MetalOffscreenTarget::render(ImDrawList* list, int w, int h) {
    @autoreleasepool {
        id<MTLCommandBuffer> cmd = r_->frameCommands();
        if (!cmd || !list || w <= 0 || h <= 0) return false;
        if (!tex_ || w != w_ || h != h_) {
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:NSUInteger(w)
                                                                                        height:NSUInteger(h)
                                                                                     mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            tex_ = [r_->device() newTextureWithDescriptor:td];  // the old one lives on in the command buffers that use it
            w_ = tex_ ? w : 0;
            h_ = tex_ ? h : 0;
            if (!tex_) return false;
        }
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = tex_;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rpd];
        ImDrawData dd;
        dd.Valid = true;
        dd.DisplayPos = ImVec2(0, 0);
        dd.DisplaySize = ImVec2(float(w), float(h));
        dd.FramebufferScale = ImVec2(1, 1);
        dd.AddDrawList(list);
        ImGui_ImplMetal_RenderDrawData(&dd, cmd, enc);  // same pixel format as the window: same pipeline
        [enc endEncoding];
        return true;
    }
}

} // namespace

std::unique_ptr<Renderer> createMetalRenderer() { return std::make_unique<MetalRenderer>(); }

} // namespace immidi

#endif  // __APPLE__
