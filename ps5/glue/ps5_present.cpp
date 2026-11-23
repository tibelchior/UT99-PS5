// PS5 frame presenter: takes the finished XRGB8888 frame SDL's dummy video driver hands over
// (patches/motorhead/0002-sdl-dummy-present.patch) and puts it on the console's display.
//
// The driver is RADV, linked into the title (no loader): every function is taken from
// vk_icdGetInstanceProcAddr. The swapchain is VK_KHR_display on the console's one display, as
// DiRT 2's ps5_vulkan.cpp does it. Per frame: copy the pixels into a staging buffer, copy that
// to an image of the frame's size, then blit it (aspect kept, black bars) onto the swapchain
// image and present.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" const char *ut_ps5_data_dir(void);  // ps5_launch.cpp

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

// What a loader-less Vulkan consumer calls (RADV's entry point, as DiRT 2's glue).
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    return vk_icdGetInstanceProcAddr(instance, name);
}

extern "C" void ut_ps5_hide_splash(void);

namespace {


// Everything goes to ut99.log while the port is brought up on hardware (create data/quiet to keep problems only).
bool log_wanted(const char *text) {
    static const bool quiet = std::getenv("UT99_QUIET") != nullptr;
    if (!quiet) return true;
    for (const char *word : {"fail", "no ", "missing", "disabled", "error", "software renderer"})
        if (std::strstr(text, word)) return true;
    return false;
}

void say(const char *format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (!log_wanted(text)) return;
    std::fprintf(stderr, "[ps5 present] %s\n", text);
    }

#define FUNCTIONS_INSTANCE(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceDisplayPropertiesKHR) X(vkGetDisplayModePropertiesKHR) \
    X(vkCreateDisplayPlaneSurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceFeatures)
#define FUNCTIONS_DEVICE(X) \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) \
    X(vkQueueSubmit) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkDestroyImage) \
    X(vkDestroyBuffer) X(vkFreeMemory) X(vkUnmapMemory) X(vkDeviceWaitIdle) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkCmdBlitImage) X(vkCmdClearColorImage)

#define DECLARE(name) PFN_##name name = nullptr;
FUNCTIONS_INSTANCE(DECLARE)
FUNCTIONS_DEVICE(DECLARE)
#undef DECLARE

struct State {
    bool tried = false, ready = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
    // The frame's own image and the staging buffer it is filled from (re-made when the size changes).
    int width = 0, height = 0;
    VkImage source = VK_NULL_HANDLE;
    VkDeviceMemory source_memory = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    unsigned long frames = 0;
    bool anisotropy = false, line_fill = false, ready_for_renderer = false;
} g;

bool load_instance() {
    auto get = vk_icdGetInstanceProcAddr;
#define LOAD(name) name = reinterpret_cast<PFN_##name>(get(g.instance, #name));
    FUNCTIONS_INSTANCE(LOAD)
#undef LOAD
    return vkEnumeratePhysicalDevices && vkCreateDevice && vkGetDeviceProcAddr;
}

bool load_device() {
#define LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(g.device, #name));
    FUNCTIONS_DEVICE(LOAD)
#undef LOAD
    return vkQueueSubmit && vkCreateSwapchainKHR && vkCmdBlitImage;
}

bool memory_type(uint32_t bits, VkMemoryPropertyFlags want, uint32_t &index) {
    for (uint32_t i = 0; i < g.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g.memory.memoryTypes[i].propertyFlags & want) == want) {
            index = i;
            return true;
        }
    return false;
}

#define CHECK(call) \
    do { \
        const VkResult r_ = (call); \
        if (r_ != VK_SUCCESS) { \
            say("%s failed: %d", #call, int(r_)); \
            return false; \
        } \
    } while (0)

// The display mode: the file "output" in /data/motorhead can name a height (1080, 1440, 2160);
// otherwise the smallest mode that holds the frame, else the largest.
uint32_t pick_mode(const VkDisplayModePropertiesKHR *modes, uint32_t count, int want_w, int want_h) {
    uint32_t forced = 0;
    const std::string output_file = std::string(ut_ps5_data_dir()) + "/output";
    if (FILE *f = std::fopen(output_file.c_str(), "r")) {
        unsigned v = 0;
        if (std::fscanf(f, "%u", &v) == 1) forced = v;
        std::fclose(f);
    }
    uint32_t best = 0, best_area = 0;
    bool have = false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &r = modes[i].parameters.visibleRegion;
        const uint32_t area = r.width * r.height;
        if (forced) {
            if (r.height == forced && (!have || modes[i].parameters.refreshRate > modes[best].parameters.refreshRate)) {
                best = i;
                have = true;
            }
        } else if (int(r.width) >= want_w && int(r.height) >= want_h && (!have || area < best_area)) {
            best = i;
            best_area = area;
            have = true;
        }
    }
    if (!have)
        for (uint32_t i = 0; i < count; ++i) {
            const auto &r = modes[i].parameters.visibleRegion;
            if (r.width * r.height > best_area) {
                best = i;
                best_area = r.width * r.height;
            }
        }
    return best;
}

bool init_display(int want_w, int want_h) {
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "UT99";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = extensions;
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create_instance) {
        say("no vkCreateInstance from the driver");
        return false;
    }
    CHECK(create_instance(&ici, nullptr, &g.instance));
    if (!load_instance()) {
        say("instance functions missing");
        return false;
    }
    uint32_t count = 1;
    VkResult r = vkEnumeratePhysicalDevices(g.instance, &count, &g.physical);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count < 1) {
        say("no physical device (%d)", int(r));
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);

    VkDisplayPropertiesKHR display{};
    count = 1;
    r = vkGetPhysicalDeviceDisplayPropertiesKHR(g.physical, &count, &display);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count < 1) {
        say("no display (%d)", int(r));
        return false;
    }
    VkDisplayModePropertiesKHR modes[16];
    count = 16;
    r = vkGetDisplayModePropertiesKHR(g.physical, display.display, &count, modes);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count < 1) {
        say("no display modes (%d)", int(r));
        return false;
    }
    for (uint32_t i = 0; i < count; ++i)
        say("display mode %u: %ux%u at %.3f Hz", i, modes[i].parameters.visibleRegion.width,
            modes[i].parameters.visibleRegion.height, modes[i].parameters.refreshRate / 1000.0);
    const uint32_t mode = pick_mode(modes, count, want_w, want_h);
    VkDisplaySurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
    sci.displayMode = modes[mode].displayMode;
    sci.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.globalAlpha = 1.0f;
    sci.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    sci.imageExtent = modes[mode].parameters.visibleRegion;
    CHECK(vkCreateDisplayPlaneSurfaceKHR(g.instance, &sci, nullptr, &g.surface));
    say("display %ux%u (mode %u) for a %dx%d frame", sci.imageExtent.width, sci.imageExtent.height, mode, want_w, want_h);

    // A queue that can transfer and present.
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &families, props.data());
    bool found = false;
    for (uint32_t i = 0; i < families && !found; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g.physical, i, g.surface, &present);
        if (present && (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            g.family = i;
            found = true;
        }
    }
    if (!found) {
        say("no queue family can present");
        return false;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // Features the Vulkan scene renderer likes (anisotropic textures, hidden-line outlines).
    VkPhysicalDeviceFeatures supported{}, enabled{};
    vkGetPhysicalDeviceFeatures(g.physical, &supported);
    enabled.samplerAnisotropy = supported.samplerAnisotropy;
    enabled.fillModeNonSolid = supported.fillModeNonSolid;
    g.anisotropy = enabled.samplerAnisotropy == VK_TRUE;
    g.line_fill = enabled.fillModeNonSolid == VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pEnabledFeatures = &enabled;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = device_extensions;
    CHECK(vkCreateDevice(g.physical, &dci, nullptr, &g.device));
    if (!load_device()) {
        say("device functions missing");
        return false;
    }
    vkGetDeviceQueue(g.device, g.family, 0, &g.queue);

    VkSurfaceCapabilitiesKHR caps{};
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical, g.surface, &caps));
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, g.surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, g.surface, &format_count, formats.data());
    VkSurfaceFormatKHR format = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (const auto &f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) format = f;
    say("swapchain format %d (of %u)", int(format.format), format_count);
    g.extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : sci.imageExtent;
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = g.surface;
    sc.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount && sc.minImageCount > caps.maxImageCount) sc.minImageCount = caps.maxImageCount;
    sc.imageFormat = format.format;
    sc.imageColorSpace = format.colorSpace;
    sc.imageExtent = g.extent;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    CHECK(vkCreateSwapchainKHR(g.device, &sc, nullptr, &g.swapchain));
    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &image_count, nullptr);
    g.images.resize(image_count);
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &image_count, g.images.data());

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g.family;
    CHECK(vkCreateCommandPool(g.device, &pci, nullptr, &g.pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    CHECK(vkAllocateCommandBuffers(g.device, &cai, &g.cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    CHECK(vkCreateFence(g.device, &fci, nullptr, &g.fence));
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CHECK(vkCreateSemaphore(g.device, &sem, nullptr, &g.acquired));
    CHECK(vkCreateSemaphore(g.device, &sem, nullptr, &g.done));
    return true;
}

bool make_frame_resources(int w, int h) {
    if (g.source) {
        vkDeviceWaitIdle(g.device);
        vkUnmapMemory(g.device, g.staging_memory);
        vkDestroyBuffer(g.device, g.staging, nullptr);
        vkFreeMemory(g.device, g.staging_memory, nullptr);
        vkDestroyImage(g.device, g.source, nullptr);
        vkFreeMemory(g.device, g.source_memory, nullptr);
        g.source = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = {uint32_t(w), uint32_t(h), 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(g.device, &ii, nullptr, &g.source));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g.device, g.source, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    if (!memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mai.memoryTypeIndex)) {
        say("no device-local memory for the frame image");
        return false;
    }
    CHECK(vkAllocateMemory(g.device, &mai, nullptr, &g.source_memory));
    CHECK(vkBindImageMemory(g.device, g.source, g.source_memory, 0));

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(w) * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHECK(vkCreateBuffer(g.device, &bi, nullptr, &g.staging));
    vkGetBufferMemoryRequirements(g.device, g.staging, &req);
    mai.allocationSize = req.size;
    if (!memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     mai.memoryTypeIndex)) {
        say("no host-visible memory for the staging buffer");
        return false;
    }
    CHECK(vkAllocateMemory(g.device, &mai, nullptr, &g.staging_memory));
    CHECK(vkBindBufferMemory(g.device, g.staging, g.staging_memory, 0));
    CHECK(vkMapMemory(g.device, g.staging_memory, 0, VK_WHOLE_SIZE, 0, &g.mapped));
    g.width = w;
    g.height = h;
    return true;
}

void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
             VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g.cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool present(const void *pixels, int w, int h, int pitch) {
    if (!g.device && !init_display(w, h)) return false;
    if ((w != g.width || h != g.height) && !make_frame_resources(w, h)) return false;

    // The previous frame's command buffer must be finished before the staging buffer is rewritten.
    CHECK(vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX));
    const auto *src = static_cast<const uint8_t *>(pixels);
    auto *dst = static_cast<uint8_t *>(g.mapped);
    if (pitch == w * 4)
        std::memcpy(dst, src, size_t(w) * h * 4);
    else
        for (int y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * w * 4, src + size_t(y) * pitch, size_t(w) * 4);

    uint32_t index = 0;
    CHECK(vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, g.acquired, VK_NULL_HANDLE, &index));
    CHECK(vkResetFences(g.device, 1, &g.fence));
    CHECK(vkResetCommandBuffer(g.cmd, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(g.cmd, &begin));

    barrier(g.source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {uint32_t(w), uint32_t(h), 1};
    vkCmdCopyBufferToImage(g.cmd, g.staging, g.source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(g.source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImage target = g.images[index];
    barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g.cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    // Aspect kept: the largest rectangle of the frame's shape that fits, centred.
    int64_t dw = g.extent.width, dh = int64_t(g.extent.width) * h / w;
    if (dh > int64_t(g.extent.height)) {
        dh = g.extent.height;
        dw = int64_t(g.extent.height) * w / h;
    }
    const int32_t x0 = int32_t((int64_t(g.extent.width) - dw) / 2), y0 = int32_t((int64_t(g.extent.height) - dh) / 2);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {w, h, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x0, y0, 0};
    blit.dstOffsets[1] = {x0 + int32_t(dw), y0 + int32_t(dh), 1};
    vkCmdBlitImage(g.cmd, g.source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   (dw == w && dh == h) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    CHECK(vkEndCommandBuffer(g.cmd));

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &g.acquired;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &g.done;
    CHECK(vkQueueSubmit(g.queue, 1, &submit, g.fence));
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &index;
    CHECK(vkQueuePresentKHR(g.queue, &pi));
    if (++g.frames == 1) {
        g.ready_for_renderer = true;
        say("first frame presented (%dx%d)", w, h);
        ut_ps5_hide_splash();
    }
    return true;
}

}  // namespace

// Called by SDL's dummy video driver for every finished frame (XRGB8888, `pitch` bytes per row).
extern "C" void ut_ps5_kb_draw(void *pixels, int w, int h, int pitch);  // ps5_keyboard.cpp
extern "C" void ut_ps5_stats_draw(void *pixels, int w, int h, int pitch);  // ps5_keyboard.cpp

extern "C" void ut_ps5_present(const void *pixels, int width, int height, int pitch) {
    if (g.tried && !g.device) return;  // initialisation failed once: do not retry every frame
    if (!g.tried) g.tried = true;
    ut_ps5_stats_draw(const_cast<void *>(pixels), width, height, pitch);  // the statistics overlay, when switched on
    ut_ps5_kb_draw(const_cast<void *>(pixels), width, height, pitch);  // the on-screen keyboard, when it is open
    if (!present(pixels, width, height, pitch) && !g.device) say("presenter disabled");
}
