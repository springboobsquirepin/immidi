// Vulkan renderer (Linux, the default there): a swap chain on the window's Vulkan surface (X11 or
// Wayland, created by GLFW) and Dear ImGui's Vulkan backend. The Vulkan loader is opened at run
// time (volk), so ImMidi still starts, with OpenGL, where no Vulkan driver is installed.
#if defined(IMMIDI_VULKAN)

#include "Renderer.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"  // includes volk.h (IMGUI_IMPL_VULKAN_USE_VOLK)

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>  // after volk.h: declares glfwCreateWindowSurface

#include "stb_image_write.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace immidi {

namespace {

constexpr uint32_t kFramesInFlight = 2;
// Dear ImGui's vertex/index buffers are used in turn by every draw (the window, then the Sound
// Canvas display): enough sets that a set is only refilled after the frame that used it finished.
constexpr uint32_t kImGuiBufferSets = 2 * kFramesInFlight + 2;

uint64_t toId(VkDescriptorSet set) { return (uint64_t)(set); }

void checkVk(VkResult r) {
    if (r < 0) fprintf(stderr, "Vulkan error %d\n", int(r));
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// An image with its memory, view and Dear ImGui texture.
struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;  // render targets
    VkDescriptorSet set = VK_NULL_HANDLE;        // ImTextureID
    int w = 0, h = 0;
};

struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* data = nullptr;
};

class VulkanRenderer;

class VulkanOffscreenTarget final : public OffscreenTarget {
public:
    explicit VulkanOffscreenTarget(VulkanRenderer* r) : r_(r) {}
    ~VulkanOffscreenTarget() override;
    bool render(ImDrawList* list, int w, int h) override;
    uint64_t texture() const override { return toId(img_.set); }
    int width() const override { return img_.w; }
    int height() const override { return img_.h; }
    void draw(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1) override;

private:
    VulkanRenderer* r_;
    GpuImage img_;
    bool failed_ = false;
};

class VulkanRenderer final : public Renderer {
public:
    RendererKind kind() const override { return RendererKind::Vulkan; }
    void windowHints() override { glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); }

    bool init(GLFWwindow* window, std::string* error) override {
        window_ = window;
        auto fail = [&](const char* why, VkResult r = VK_SUCCESS) {
            if (error) {
                *error = why;
                if (r != VK_SUCCESS) *error += " (VkResult " + std::to_string(int(r)) + ")";
            }
            return false;
        };
        if (volkInitialize() != VK_SUCCESS) return fail("the Vulkan loader (libvulkan.so.1) is not installed");
        if (!glfwVulkanSupported()) return fail("GLFW cannot use the Vulkan loader");
        uint32_t extCount = 0;
        const char** exts = glfwGetRequiredInstanceExtensions(&extCount);
        if (!exts) return fail("Vulkan cannot draw to windows of this window system");

        VkApplicationInfo app = {};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "ImMidi";
        app.pEngineName = "Dear ImGui";
        app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = extCount;
        ici.ppEnabledExtensionNames = exts;
        VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
        if (r != VK_SUCCESS) return fail("vkCreateInstance failed", r);
        volkLoadInstance(instance_);
        r = glfwCreateWindowSurface(instance_, window, nullptr, &surface_);
        if (r != VK_SUCCESS) return fail("the window surface could not be created", r);
        if (!pickDevice()) return fail("no Vulkan device can draw to this window");

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qi = {};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family_;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci = {};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qi;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = devExts;
        r = vkCreateDevice(phys_, &dci, nullptr, &device_);
        if (r != VK_SUCCESS) return fail("vkCreateDevice failed", r);
        volkLoadDevice(device_);
        vkGetDeviceQueue(device_, family_, 0, &queue_);

        pickSurfaceFormat();
        // Same attachment and dependencies, different final layout: the passes are compatible, so
        // Dear ImGui's pipeline draws in both.
        mainPass_ = createRenderPass(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        offscreenPass_ = createRenderPass(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!mainPass_ || !offscreenPass_) return fail("vkCreateRenderPass failed");

        for (uint32_t i = 0; i < kFramesInFlight; i++) {
            VkCommandPoolCreateInfo pi = {};
            pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pi.queueFamilyIndex = family_;
            VkFenceCreateInfo fi = {};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo si = {};
            si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateCommandPool(device_, &pi, nullptr, &pools_[i]) != VK_SUCCESS || !allocateCommandBuffer(pools_[i], &cmds_[i]) ||
                vkCreateFence(device_, &fi, nullptr, &fences_[i]) != VK_SUCCESS || vkCreateSemaphore(device_, &si, nullptr, &acquired_[i]) != VK_SUCCESS)
                return fail("frame resources could not be created");
        }
        VkCommandPoolCreateInfo upi = {};
        upi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        upi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        upi.queueFamilyIndex = family_;
        VkFenceCreateInfo ufi = {};
        ufi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateCommandPool(device_, &upi, nullptr, &uploadPool_) != VK_SUCCESS || !allocateCommandBuffer(uploadPool_, &uploadCmd_) ||
            vkCreateFence(device_, &ufi, nullptr, &uploadFence_) != VK_SUCCESS)
            return fail("upload resources could not be created");

        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        if (w > 0 && h > 0 && !createSwapchain(w, h)) return fail("the swap chain could not be created");
        return true;
    }

    bool initImGui() override {
        if (!ImGui_ImplGlfw_InitForVulkan(window_, true)) return false;
        ImGui_ImplVulkan_InitInfo ii = {};
        ii.ApiVersion = VK_API_VERSION_1_0;
        ii.Instance = instance_;
        ii.PhysicalDevice = phys_;
        ii.Device = device_;
        ii.QueueFamily = family_;
        ii.Queue = queue_;
        ii.DescriptorPoolSize = 64;  // font atlas pages, keyboard images, the Sound Canvas display
        ii.MinImageCount = 2;
        ii.ImageCount = kImGuiBufferSets;
        ii.PipelineInfoMain.RenderPass = mainPass_;
        ii.PipelineInfoMain.Subpass = 0;
        ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ii.CheckVkResultFn = checkVk;
        imguiReady_ = ImGui_ImplVulkan_Init(&ii);
        return imguiReady_;
    }

    void shutdownImGui() override {
        if (device_) vkDeviceWaitIdle(device_);
        // Dear ImGui's descriptor sets go with its pool: free the textures first.
        collectRetired(true);
        for (GpuImage& img : pixelTextures_) destroyImage(img);
        pixelTextures_.clear();
        if (imguiReady_) ImGui_ImplVulkan_Shutdown();
        imguiReady_ = false;
        ImGui_ImplGlfw_Shutdown();
    }

    void shutdown() override {
        if (device_) {
            vkDeviceWaitIdle(device_);
            collectRetired(true);
            for (GpuImage& img : pixelTextures_) destroyImage(img);
            pixelTextures_.clear();
            destroySwapchainResources();
            if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
            for (VkSemaphore s : renderDone_) vkDestroySemaphore(device_, s, nullptr);
            renderDone_.clear();
            for (uint32_t i = 0; i < kFramesInFlight; i++) {
                if (fences_[i]) vkDestroyFence(device_, fences_[i], nullptr);
                if (acquired_[i]) vkDestroySemaphore(device_, acquired_[i], nullptr);
                if (pools_[i]) vkDestroyCommandPool(device_, pools_[i], nullptr);
                fences_[i] = VK_NULL_HANDLE;
                acquired_[i] = VK_NULL_HANDLE;
                pools_[i] = VK_NULL_HANDLE;
            }
            if (uploadFence_) vkDestroyFence(device_, uploadFence_, nullptr);
            if (uploadPool_) vkDestroyCommandPool(device_, uploadPool_, nullptr);
            if (mainPass_) vkDestroyRenderPass(device_, mainPass_, nullptr);
            if (offscreenPass_) vkDestroyRenderPass(device_, offscreenPass_, nullptr);
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        surface_ = VK_NULL_HANDLE;
        instance_ = VK_NULL_HANDLE;
    }

    void newFrame() override { ImGui_ImplVulkan_NewFrame(); }

    void render(ImDrawData* data, const float c[4]) override {
        recording_ = frameSubmitted_ = false;
        if (!device_ || lost_) return;
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        if (w <= 0 || h <= 0) return;
        if (!swapchain_ || recreate_ || uint32_t(w) != extent_.width || uint32_t(h) != extent_.height)
            if (!createSwapchain(w, h)) return;
        if (!ok(vkWaitForFences(device_, 1, &fences_[slot_], VK_TRUE, UINT64_MAX))) return;
        collectRetired(false);
        VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_[slot_], VK_NULL_HANDLE, &imageIndex_);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_ = true;  // next frame
            return;
        }
        if (r == VK_SUBOPTIMAL_KHR) recreate_ = true;  // still presentable
        else if (!ok(r)) return;
        vkResetFences(device_, 1, &fences_[slot_]);
        vkResetCommandPool(device_, pools_[slot_], 0);
        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmds_[slot_], &bi);
        recording_ = true;  // from here on the frame is always submitted (present / screenshot)

        VkClearValue clear = {};
        clear.color = {{c[0], c[1], c[2], c[3]}};
        VkRenderPassBeginInfo rp = {};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = mainPass_;
        rp.framebuffer = framebuffers_[imageIndex_];
        rp.renderArea.extent = extent_;
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cmds_[slot_], &rp, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(data, cmds_[slot_]);
        vkCmdEndRenderPass(cmds_[slot_]);
    }

    void present() override {
        if (recording_) {
            if (!frameSubmitted_) submitFrame();
            if (!lost_) {
                VkPresentInfoKHR pi = {};
                pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                pi.waitSemaphoreCount = 1;
                pi.pWaitSemaphores = &renderDone_[imageIndex_];
                pi.swapchainCount = 1;
                pi.pSwapchains = &swapchain_;
                pi.pImageIndices = &imageIndex_;
                VkResult r = vkQueuePresentKHR(queue_, &pi);  // FIFO: waits for the display (vsync)
                if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_ = true;
                else ok(r);
            }
            slot_ = (slot_ + 1) % kFramesInFlight;
        }
        recording_ = frameSubmitted_ = false;
        frame_++;
    }

    bool screenshot(const std::string& path) override {
        if (!recording_ || frameSubmitted_ || !canCopy_) return false;
        VkCommandBuffer cmd = cmds_[slot_];
        VkImage image = images_[imageIndex_];
        const uint32_t w = extent_.width, h = extent_.height;
        HostBuffer buf;
        if (!createHostBuffer(buf, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
        // The render pass hands the image over to the transfer stage (see createRenderPass).
        imageBarrier(cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &region);
        imageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        VkMemoryBarrier mb = {};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        bool done = submitFrame() && ok(vkWaitForFences(device_, 1, &fences_[slot_], VK_TRUE, UINT64_MAX));
        if (done) {
            std::vector<unsigned char> px(size_t(w) * h * 4);
            std::memcpy(px.data(), buf.data, px.size());
            if (format_.format == VK_FORMAT_B8G8R8A8_UNORM || format_.format == VK_FORMAT_B8G8R8A8_SRGB)
                for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
            done = stbi_write_png(path.c_str(), int(w), int(h), 4, px.data(), int(w) * 4) != 0;
        }
        destroyHostBuffer(buf);
        return done;
    }

    std::unique_ptr<OffscreenTarget> createOffscreenTarget() override { return std::make_unique<VulkanOffscreenTarget>(this); }

    uint64_t uploadPixelTexture(uint64_t texture, int w, int h, const uint32_t* rgba) override {
        deletePixelTexture(texture);
        if (!device_ || !imguiReady_ || lost_ || w <= 0 || h <= 0) return 0;
        GpuImage img;
        HostBuffer staging;
        const VkDeviceSize bytes = VkDeviceSize(w) * VkDeviceSize(h) * 4;
        bool good = createImage(img, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
                    createHostBuffer(staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (good) {
            std::memcpy(staging.data, rgba, size_t(bytes));
            good = submitNow([&](VkCommandBuffer cmd) {
                imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy region = {};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {uint32_t(w), uint32_t(h), 1};
                vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            });
        }
        destroyHostBuffer(staging);
        if (good) img.set = ImGui_ImplVulkan_AddTexture(img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!good || !img.set) {
            destroyImage(img);  // never used by the GPU
            return 0;
        }
        pixelTextures_.push_back(img);
        return toId(img.set);
    }

    // Freed once the frames that may draw it have finished.
    void deletePixelTexture(uint64_t texture) override {
        for (size_t i = 0; i < pixelTextures_.size(); i++)
            if (toId(pixelTextures_[i].set) == texture) {
                retire(pixelTextures_[i]);
                pixelTextures_.erase(pixelTextures_.begin() + long(i));
                return;
            }
    }

    // --- used by VulkanOffscreenTarget

    // Renders `list` into `img` (recreated at w x h when needed) in the frame's command buffer. The
    // pass leaves it ready for the fragment shader (it is drawn in the next frame).
    bool renderOffscreen(GpuImage& img, bool& failed, ImDrawList* list, int w, int h) {
        if (!recording_ || frameSubmitted_ || failed || !list || w <= 0 || h <= 0) return false;
        if (!img.image || img.w != w || img.h != h) {
            retire(img);
            bool good = createImage(img, w, h, format_.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            if (good) {
                VkFramebufferCreateInfo fi = {};
                fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                fi.renderPass = offscreenPass_;
                fi.attachmentCount = 1;
                fi.pAttachments = &img.view;
                fi.width = uint32_t(w);
                fi.height = uint32_t(h);
                fi.layers = 1;
                good = vkCreateFramebuffer(device_, &fi, nullptr, &img.framebuffer) == VK_SUCCESS;
            }
            if (good) img.set = ImGui_ImplVulkan_AddTexture(img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (!good || !img.set) {
                destroyImage(img);
                failed = true;
                return false;
            }
        }
        VkCommandBuffer cmd = cmds_[slot_];
        VkClearValue clear = {};
        VkRenderPassBeginInfo rp = {};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = offscreenPass_;
        rp.framebuffer = img.framebuffer;
        rp.renderArea.extent = {uint32_t(w), uint32_t(h)};
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        ImDrawData dd;
        dd.Valid = true;
        dd.DisplayPos = ImVec2(0, 0);
        dd.DisplaySize = ImVec2(float(w), float(h));
        dd.FramebufferScale = ImVec2(1, 1);
        dd.AddDrawList(list);
        ImGui_ImplVulkan_RenderDrawData(&dd, cmd);
        vkCmdEndRenderPass(cmd);
        return true;
    }

    // Destroyed once the frames that may use it have finished.
    void retire(GpuImage& img) {
        if (img.image || img.set) retired_.push_back({frame_, img});
        img = GpuImage();
    }

private:
    struct Retired {
        uint64_t frame;
        GpuImage img;
    };

    bool ok(VkResult r) {
        if (r == VK_SUCCESS) return true;
        if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR) {
            if (!lost_) fprintf(stderr, "Vulkan: the device or window surface was lost (VkResult %d); restart ImMidi\n", int(r));
            lost_ = true;
        } else if (r < 0) {
            fprintf(stderr, "Vulkan error %d\n", int(r));
        }
        return false;
    }

    bool pickDevice() {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        std::vector<VkPhysicalDevice> devices(n);
        if (n) vkEnumeratePhysicalDevices(instance_, &n, devices.data());
        int best = -1;
        for (VkPhysicalDevice dev : devices) {
            uint32_t en = 0;
            vkEnumerateDeviceExtensionProperties(dev, nullptr, &en, nullptr);
            std::vector<VkExtensionProperties> exts(en);
            if (en) vkEnumerateDeviceExtensionProperties(dev, nullptr, &en, exts.data());
            bool swapchain = false;
            for (const VkExtensionProperties& e : exts) swapchain |= std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            if (!swapchain) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> families(qn);
            if (qn) vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, families.data());
            int family = -1;
            for (uint32_t i = 0; i < qn && family < 0; i++) {
                VkBool32 present = VK_FALSE;
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &present) == VK_SUCCESS && present)
                    family = int(i);
            }
            if (family < 0) continue;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev, &props);
            // A GPU before the software rasterizer.
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 4
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 2
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 1
                                                                                     : 0;
            if (score > best) {
                best = score;
                phys_ = dev;
                family_ = uint32_t(family);
            }
        }
        return best >= 0;
    }

    void pickSurfaceFormat() {
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        if (n) vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, formats.data());
        // Dear ImGui's colours are meant for a UNORM (non-sRGB) target.
        format_ = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        if (n == 0 || (n == 1 && formats[0].format == VK_FORMAT_UNDEFINED)) return;
        for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
            for (const VkSurfaceFormatKHR& f : formats)
                if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    format_ = f;
                    return;
                }
        format_ = formats[0];
    }

    // One colour attachment cleared at the start. The dependencies are the same for the window and
    // the offscreen passes (so the passes are compatible): the attachment is written after earlier
    // colour output and fragment shader reads (the acquired image; the display texture, drawn in the
    // window earlier in the frame), and handed over to fragment shaders and transfers at the end (the
    // display texture in the next frame; screenshots).
    VkRenderPass createRenderPass(VkImageLayout finalLayout) {
        VkAttachmentDescription att = {};
        att.format = format_.format;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = finalLayout;
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency deps[2] = {};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = 0;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo ci = {};
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments = &att;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        VkRenderPass pass = VK_NULL_HANDLE;
        vkCreateRenderPass(device_, &ci, nullptr, &pass);
        return pass;
    }

    bool createSwapchain(int w, int h) {
        vkDeviceWaitIdle(device_);
        VkSurfaceCapabilitiesKHR caps;
        if (!ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps))) return false;
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {  // the window follows the swap chain (Wayland)
            extent.width = std::clamp(uint32_t(w), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(uint32_t(h), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) return false;  // minimized
        uint32_t count = std::max(caps.minImageCount + 1, 2u);
        if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                alpha = a;
                break;
            }
        VkSwapchainCreateInfoKHR ci = {};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface_;
        ci.minImageCount = count;
        ci.imageFormat = format_.format;
        ci.imageColorSpace = format_.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);  // + screenshots
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
        ci.compositeAlpha = alpha;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync, always supported
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = swapchain_;
        VkSwapchainKHR next = VK_NULL_HANDLE;
        VkResult r = vkCreateSwapchainKHR(device_, &ci, nullptr, &next);
        destroySwapchainResources();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
        if (!ok(r)) return false;
        swapchain_ = next;
        extent_ = extent;
        canCopy_ = (ci.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        uint32_t n = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
        images_.resize(n);
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());
        for (VkImage image : images_) {
            VkImageView view = createView(image, format_.format);
            VkFramebufferCreateInfo fi = {};
            fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass = mainPass_;
            fi.attachmentCount = 1;
            fi.pAttachments = &view;
            fi.width = extent.width;
            fi.height = extent.height;
            fi.layers = 1;
            VkFramebuffer fb = VK_NULL_HANDLE;
            if (view) vkCreateFramebuffer(device_, &fi, nullptr, &fb);
            views_.push_back(view);
            framebuffers_.push_back(fb);
            if (!view || !fb) {
                recreate_ = true;
                return false;
            }
        }
        // One "rendered" semaphore per image (reused when the image comes back); kept across resizes.
        while (renderDone_.size() < images_.size()) {
            VkSemaphoreCreateInfo si = {};
            si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkSemaphore s = VK_NULL_HANDLE;
            if (vkCreateSemaphore(device_, &si, nullptr, &s) != VK_SUCCESS) return false;
            renderDone_.push_back(s);
        }
        recreate_ = false;
        return true;
    }

    void destroySwapchainResources() {
        for (VkFramebuffer fb : framebuffers_)
            if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        for (VkImageView v : views_)
            if (v) vkDestroyImageView(device_, v, nullptr);
        framebuffers_.clear();
        views_.clear();
        images_.clear();
    }

    bool submitFrame() {
        VkCommandBuffer cmd = cmds_[slot_];
        const bool recorded = ok(vkEndCommandBuffer(cmd));
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired_[slot_];
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = recorded ? 1 : 0;  // still consume the semaphore and signal the fence
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderDone_[imageIndex_];
        const bool submitted = ok(vkQueueSubmit(queue_, 1, &si, fences_[slot_]));
        if (!submitted) lost_ = true;  // the fence would never be signalled
        frameSubmitted_ = true;
        slotFrame_[slot_] = frame_;
        submitted_ = frame_;
        return recorded && submitted;
    }

    // Records and runs `record` at once (uploads).
    template <typename F>
    bool submitNow(F record) {
        vkResetCommandBuffer(uploadCmd_, 0);
        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(uploadCmd_, &bi) != VK_SUCCESS) return false;
        record(uploadCmd_);
        if (vkEndCommandBuffer(uploadCmd_) != VK_SUCCESS) return false;
        vkResetFences(device_, 1, &uploadFence_);
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &uploadCmd_;
        return ok(vkQueueSubmit(queue_, 1, &si, uploadFence_)) && ok(vkWaitForFences(device_, 1, &uploadFence_, VK_TRUE, UINT64_MAX));
    }

    bool allocateCommandBuffer(VkCommandPool pool, VkCommandBuffer* cmd) {
        VkCommandBufferAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        return vkAllocateCommandBuffers(device_, &ai, cmd) == VK_SUCCESS;
    }

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        return UINT32_MAX;
    }

    VkImageView createView(VkImage image, VkFormat format) {
        VkImageViewCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(device_, &vi, nullptr, &view);
        return view;
    }

    bool createImage(GpuImage& img, int w, int h, VkFormat format, VkImageUsageFlags usage) {
        VkImageCreateInfo ci = {};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {uint32_t(w), uint32_t(h), 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ci, nullptr, &img.image) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, img.image, &req);
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = memoryType(req.memoryTypeBits, 0);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ai, nullptr, &img.memory) != VK_SUCCESS ||
            vkBindImageMemory(device_, img.image, img.memory, 0) != VK_SUCCESS)
            return false;
        img.view = createView(img.image, format);
        img.w = w;
        img.h = h;
        return img.view != VK_NULL_HANDLE;
    }

    void destroyImage(GpuImage& img) {
        if (img.set && imguiReady_) ImGui_ImplVulkan_RemoveTexture(img.set);
        if (img.framebuffer) vkDestroyFramebuffer(device_, img.framebuffer, nullptr);
        if (img.view) vkDestroyImageView(device_, img.view, nullptr);
        if (img.image) vkDestroyImage(device_, img.image, nullptr);
        if (img.memory) vkFreeMemory(device_, img.memory, nullptr);
        img = GpuImage();
    }

    bool createHostBuffer(HostBuffer& b, VkDeviceSize size, VkBufferUsageFlags usage) {
        VkBufferCreateInfo ci = {};
        ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size = size;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &ci, nullptr, &b.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, b.buffer, &req);
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ai, nullptr, &b.memory) != VK_SUCCESS ||
            vkBindBufferMemory(device_, b.buffer, b.memory, 0) != VK_SUCCESS || vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.data) != VK_SUCCESS) {
            destroyHostBuffer(b);
            return false;
        }
        return true;
    }

    void destroyHostBuffer(HostBuffer& b) {
        if (b.data) vkUnmapMemory(device_, b.memory);
        if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
        b = HostBuffer();
    }

    // True when every frame up to `frame` has finished on the GPU: a slot's frames are finished
    // when the slot was reused by a later frame (after waiting for its fence) or its fence is set.
    bool frameDone(uint64_t frame) const {
        if (frame > submitted_) return false;
        for (uint32_t s = 0; s < kFramesInFlight; s++)
            if (slotFrame_[s] <= frame && vkGetFenceStatus(device_, fences_[s]) != VK_SUCCESS) return false;
        return true;
    }

    void collectRetired(bool all) {
        for (size_t i = 0; i < retired_.size();) {
            if (all || frameDone(retired_[i].frame)) {
                destroyImage(retired_[i].img);
                retired_.erase(retired_.begin() + long(i));
            } else {
                i++;
            }
        }
    }

    GLFWwindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format_ = {};
    VkRenderPass mainPass_ = VK_NULL_HANDLE, offscreenPass_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_ = {0, 0};
    bool canCopy_ = false;  // swap chain images can be read back (screenshots)
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> renderDone_;
    bool recreate_ = false;

    VkCommandPool pools_[kFramesInFlight] = {};
    VkCommandBuffer cmds_[kFramesInFlight] = {};
    VkFence fences_[kFramesInFlight] = {};
    VkSemaphore acquired_[kFramesInFlight] = {};
    uint64_t slotFrame_[kFramesInFlight] = {};  // the last frame submitted with each slot
    uint32_t slot_ = 0, imageIndex_ = 0;
    uint64_t frame_ = 1, submitted_ = 0;
    bool recording_ = false, frameSubmitted_ = false, lost_ = false;

    VkCommandPool uploadPool_ = VK_NULL_HANDLE;
    VkCommandBuffer uploadCmd_ = VK_NULL_HANDLE;
    VkFence uploadFence_ = VK_NULL_HANDLE;

    std::vector<GpuImage> pixelTextures_;
    std::vector<Retired> retired_;
    bool imguiReady_ = false;
};

VulkanOffscreenTarget::~VulkanOffscreenTarget() { r_->retire(img_); }

bool VulkanOffscreenTarget::render(ImDrawList* list, int w, int h) { return r_->renderOffscreen(img_, failed_, list, w, h); }

void VulkanOffscreenTarget::draw(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1) {
    dl->AddImage(ImTextureRef(ImTextureID(texture())), p0, p1);  // one level: Dear ImGui's linear sampler is bilinear
}

} // namespace

std::unique_ptr<Renderer> createVulkanRenderer() { return std::make_unique<VulkanRenderer>(); }

} // namespace immidi

#endif  // IMMIDI_VULKAN
