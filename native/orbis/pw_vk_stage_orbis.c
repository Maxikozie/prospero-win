/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The PS4 port's milestone e1 Vulkan stages in the title (pw_vk_stage_orbis.h).
 * Every entry point comes from libvulkan.prx's vkGetInstanceProcAddr and
 * vkGetDeviceProcAddr, so the present counters there see this program as
 * they will see DXVK. */
#define VK_NO_PROTOTYPES   /* the title links no Vulkan: every call goes through a looked-up pointer */
#include "pw_vk_stage_orbis.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

extern int sceKernelAvailableFlexibleMemorySize(size_t *size);
extern int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t alignment, int64_t *start_out,
                                              size_t *size_out);
extern size_t sceKernelGetDirectMemorySize(void);

enum { WIDTH = 1920, HEIGHT = 1080, MARK = 64, MAX_IMAGES = 8 };
static const uint64_t TIMEOUT_NS = 2000000000ull;   /* a fence or an acquire this late is a GPU hang */

static PwVkHost host;
static void *library;
static PFN_vkGetInstanceProcAddr gipa;
static int (*read_stats)(struct pw_vulkan_stats *);
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;   /* kept after vk_present: Wine starts beside a live device and arena */

static void vstep(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void vstep(const char *format, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    if (host.step) host.step(line);
}

/* The largest free block of direct memory (the kernel reports no total) and
 * the free flexible memory, in MiB. */
static unsigned long long dmem_free_mib(void)
{
    int64_t start = 0;
    size_t size = 0;
    if (sceKernelAvailableDirectMemorySize(0, (int64_t)sceKernelGetDirectMemorySize(), 0x4000, &start, &size)) return 0;
    return (unsigned long long)(size >> 20);
}

static unsigned long long flex_free_mib(void)
{
    size_t size = 0;
    if (sceKernelAvailableFlexibleMemorySize(&size)) return 0;
    return (unsigned long long)(size >> 20);
}

void pw_vk_memory(unsigned long long *dmem_mib, unsigned long long *flex_mib)
{
    *dmem_mib = dmem_free_mib();
    *flex_mib = flex_free_mib();
}

int pw_vk_open(const PwVkHost *config, char *why, size_t size)
{
    if (gipa) return 0;
    host = *config;
    if (!host.dlopen || !host.dlsym) { snprintf(why, size, "nohost"); return -1; }
    if (!(library = host.dlopen("libvulkan.so", 0))) { snprintf(why, size, "dlopen"); return -1; }
    gipa = (PFN_vkGetInstanceProcAddr)(uintptr_t)host.dlsym(library, "vkGetInstanceProcAddr");
    read_stats = (int (*)(struct pw_vulkan_stats *))(uintptr_t)host.dlsym(library, "pw_vulkan_stats");
    if (!gipa) { snprintf(why, size, "nosym"); return -1; }
    return 0;
}

int pw_vk_stats(struct pw_vulkan_stats *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->size = sizeof(*stats);
    return read_stats ? read_stats(stats) : -1;
}

#define GLOBAL_FN(name) PFN_##name name = (PFN_##name)gipa(VK_NULL_HANDLE, #name)
#define INSTANCE_FN(name) PFN_##name name = (PFN_##name)gipa(instance, #name)

/* The instance and the first physical device, once per process (RADV's
 * winsys carves its arena while the devices are enumerated). */
static int make_instance(char *why, size_t size)
{
    static const char *const extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "ps4-wine e1",
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                  .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions };
    uint32_t count = 1;
    VkResult r;

    if (physical) return 0;
    if (!gipa) { snprintf(why, size, "notloaded"); return -1; }
    {
        GLOBAL_FN(vkCreateInstance);
        if (!vkCreateInstance) { snprintf(why, size, "nosym"); return -1; }
        if (!instance && (r = vkCreateInstance(&info, NULL, &instance))) {
            snprintf(why, size, "vkCreateInstance:%d", (int)r);
            return -1;
        }
    }
    {
        INSTANCE_FN(vkEnumeratePhysicalDevices);
        r = vkEnumeratePhysicalDevices(instance, &count, &physical);
        if ((r && r != VK_INCOMPLETE) || !count || !physical) {
            snprintf(why, size, "vkEnumeratePhysicalDevices:%d:%u", (int)r, count);
            physical = VK_NULL_HANDLE;
            return -1;
        }
    }
    return 0;
}

/* deviceName with anything but [A-Za-z0-9.-] as '_', for a key=value line. */
static void field_text(const char *in, char *out, size_t size)
{
    size_t n = 0;
    for (; *in && n + 1 < size; in++, n++) {
        char c = *in;
        out[n] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'
                     ? c : '_';
    }
    out[n] = 0;
}

void pw_vk_caps(char *fields, size_t size)
{
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT };
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .pNext = &xfb };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = &gpl };
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, .pNext = &robustness2 };
    VkPhysicalDeviceVulkan13Features v13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                             .pNext = &maintenance5 };
    VkPhysicalDeviceFeatures2 features = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v13 };
    VkPhysicalDeviceMaintenance3Properties m3 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES };
    VkPhysicalDeviceProperties2 props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m3 };
    VkPhysicalDeviceMemoryProperties memory;
    unsigned long long dmem_before = dmem_free_mib(), flex_before = flex_free_mib();
    uint64_t t0 = host.now_ns ? host.now_ns() : 0;
    uint32_t exts = 0, api;
    char why[96], name[96];

    if (make_instance(why, sizeof(why))) {
        snprintf(fields, size, "inst=fail:%s dmem_before_mib=%llu dmem_after_mib=%llu", why, dmem_before,
                 dmem_free_mib());
        return;
    }
    {
        INSTANCE_FN(vkGetPhysicalDeviceProperties2);
        INSTANCE_FN(vkGetPhysicalDeviceFeatures2);
        INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
        INSTANCE_FN(vkEnumerateDeviceExtensionProperties);
        vkGetPhysicalDeviceProperties2(physical, &props);
        vkGetPhysicalDeviceFeatures2(physical, &features);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        vkEnumerateDeviceExtensionProperties(physical, NULL, &exts, NULL);
    }
    api = props.properties.apiVersion;
    field_text(props.properties.deviceName, name, sizeof(name));
    vstep("vk_caps device=%s api=%u.%u.%u driver=0x%x vendor=0x%x id=0x%x exts=%u heaps=%u", name,
          VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
          props.properties.driverVersion, props.properties.vendorID, props.properties.deviceID, exts,
          memory.memoryHeapCount);
    for (uint32_t i = 0; i < memory.memoryHeapCount; i++)
        vstep("vk_caps heap %u size_mib=%llu flags=0x%x", i, (unsigned long long)(memory.memoryHeaps[i].size >> 20),
              memory.memoryHeaps[i].flags);
    snprintf(fields, size, "inst=ok api=%u.%u device=%s maintenance5=%d robustness2=%d gpl=%d dyn_rendering=%d xfb=%d "
             "exts=%u heap0_mib=%llu max_alloc_mib=%llu dmem_before_mib=%llu dmem_after_mib=%llu "
             "flex_before_mib=%llu flex_after_mib=%llu caps_ms=%llu",
             VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), name, maintenance5.maintenance5 == VK_TRUE,
             robustness2.robustBufferAccess2 && robustness2.nullDescriptor, gpl.graphicsPipelineLibrary == VK_TRUE,
             v13.dynamicRendering == VK_TRUE, xfb.transformFeedback && xfb.geometryStreams, exts,
             memory.memoryHeapCount ? (unsigned long long)(memory.memoryHeaps[0].size >> 20) : 0ull,
             (unsigned long long)(m3.maxMemoryAllocationSize >> 20), dmem_before, dmem_free_mib(), flex_before,
             flex_free_mib(), host.now_ns ? (unsigned long long)((host.now_ns() - t0) / 1000000ull) : 0ull);
}

/* ---- vk_present ------------------------------------------------------------------- */

/* The device-level entry points vk_present uses, through libvulkan's
 * vkGetDeviceProcAddr (its counting wrappers for swapchains and presents). */
#define DEVICE_FNS(X) \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkDeviceWaitIdle) X(vkCreateCommandPool) \
    X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdCopyBufferToImage) X(vkCreateSemaphore) \
    X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory)
#define DECLARE_FN(name) static PFN_##name name;
DEVICE_FNS(DECLARE_FN)

typedef struct Present {
    VkSurfaceKHR surface;
    VkSurfaceFormatKHR format;
    uint32_t family, image_count;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkImage images[MAX_IMAGES];
    VkCommandPool pool;
    VkCommandBuffer commands[MAX_IMAGES];
    VkSemaphore acquired, rendered[MAX_IMAGES];
    VkFence fence;
    VkBuffer mark;
    char failed[96];
    int hang;
} Present;

static int failed(Present *p, const char *call, VkResult r)
{
    if (!p->failed[0]) snprintf(p->failed, sizeof(p->failed), "%s:%d", call, (int)r);
    return -1;
}

static int make_device(Present *p)
{
    static const char *const extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkQueueFamilyProperties families[16];
    uint32_t count = 16;
    float priority = 1.0f;
    VkBool32 supported = VK_FALSE;
    VkResult r;
    INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
    INSTANCE_FN(vkGetPhysicalDeviceSurfaceSupportKHR);
    INSTANCE_FN(vkCreateDevice);
    INSTANCE_FN(vkGetDeviceProcAddr);

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    for (p->family = 0; p->family < count; p->family++)
        if (families[p->family].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    if (p->family == count) return failed(p, "nographicsqueue", VK_ERROR_FEATURE_NOT_PRESENT);
    if ((r = vkGetPhysicalDeviceSurfaceSupportKHR(physical, p->family, p->surface, &supported)) || !supported)
        return failed(p, "vkGetPhysicalDeviceSurfaceSupportKHR", r ? r : VK_ERROR_SURFACE_LOST_KHR);
    if (!device) {
        VkDeviceQueueCreateInfo queue = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueFamilyIndex = p->family, .queueCount = 1,
                                          .pQueuePriorities = &priority };
        VkDeviceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &queue, .enabledExtensionCount = 1,
                                    .ppEnabledExtensionNames = extensions };
        if ((r = vkCreateDevice(physical, &info, NULL, &device))) return failed(p, "vkCreateDevice", r);
    }
#define LOAD_FN(name) \
    if (!(name = (PFN_##name)vkGetDeviceProcAddr(device, #name))) return failed(p, "no " #name, VK_ERROR_INCOMPATIBLE_DRIVER);
    DEVICE_FNS(LOAD_FN)
#undef LOAD_FN
    vkGetDeviceQueue(device, p->family, 0, &p->queue);
    return 0;
}

/* The orange square's bytes, R,G,B,A per pixel, in a host-visible buffer. */
static int make_mark(Present *p)
{
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MARK * MARK * 4,
                                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    const uint8_t rgba[4] = { PW_VK_E1_MARK_RGB, 255 };
    VkPhysicalDeviceMemoryProperties memory;
    VkMemoryRequirements needs;
    VkDeviceMemory backing;
    uint8_t *bytes;
    uint32_t type;
    VkResult r;
    INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);

    if ((r = vkCreateBuffer(device, &info, NULL, &p->mark))) return failed(p, "vkCreateBuffer", r);
    vkGetBufferMemoryRequirements(device, p->mark, &needs);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (type = 0; type < memory.memoryTypeCount; type++) {
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((needs.memoryTypeBits & (1u << type)) && (memory.memoryTypes[type].propertyFlags & want) == want) break;
    }
    if (type == memory.memoryTypeCount) return failed(p, "nohostmemory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    {
        VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                          .allocationSize = needs.size, .memoryTypeIndex = type };
        if ((r = vkAllocateMemory(device, &allocate, NULL, &backing))) return failed(p, "vkAllocateMemory", r);
    }
    if ((r = vkBindBufferMemory(device, p->mark, backing, 0))) return failed(p, "vkBindBufferMemory", r);
    if ((r = vkMapMemory(device, backing, 0, VK_WHOLE_SIZE, 0, (void **)&bytes))) return failed(p, "vkMapMemory", r);
    for (unsigned i = 0; i < MARK * MARK; i++) memcpy(bytes + i * 4, rgba, 4);
    return 0;
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src_access,
                               .dstAccessMask = dst_access, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                               .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* A swapchain at 1920x1080 and one recorded frame per image: the clear,
 * then the square copied over it, then the present layout. */
static int make_swapchain(Present *p)
{
    VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = p->surface, .imageFormat = p->format.format,
        .imageColorSpace = p->format.colorSpace, .imageExtent = { WIDTH, HEIGHT }, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE, .minImageCount = p->image_count };
    VkCommandBufferAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = p->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY };
    VkClearColorValue clear = { .float32 = { PW_VK_E1_CLEAR_RGB, 255 } };
    const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    const VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                       .imageOffset = { PW_VK_E1_MARK_X - MARK / 2, PW_VK_E1_MARK_Y - MARK / 2, 0 },
                                       .imageExtent = { MARK, MARK, 1 } };
    uint32_t count = MAX_IMAGES;
    VkResult r;

    for (unsigned i = 0; i < 4; i++) clear.float32[i] /= 255.0f;
    if ((r = vkCreateSwapchainKHR(device, &info, NULL, &p->swapchain))) return failed(p, "vkCreateSwapchainKHR", r);
    if ((r = vkGetSwapchainImagesKHR(device, p->swapchain, &count, p->images)) && r != VK_INCOMPLETE)
        return failed(p, "vkGetSwapchainImagesKHR", r);
    p->image_count = count;
    if ((r = vkResetCommandPool(device, p->pool, 0))) return failed(p, "vkResetCommandPool", r);
    allocate.commandBufferCount = MAX_IMAGES;
    if (!p->commands[0] && (r = vkAllocateCommandBuffers(device, &allocate, p->commands)))
        return failed(p, "vkAllocateCommandBuffers", r);
    for (uint32_t i = 0; i < count; i++) {
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkCommandBuffer cmd = p->commands[i];
        if ((r = vkBeginCommandBuffer(cmd, &begin))) return failed(p, "vkBeginCommandBuffer", r);
        barrier(cmd, p->images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(cmd, p->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        barrier(cmd, p->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(cmd, p->mark, p->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(cmd, p->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        if ((r = vkEndCommandBuffer(cmd))) return failed(p, "vkEndCommandBuffer", r);
    }
    return 0;
}

/* FRAMES frames, one in flight; a fence or an acquire not back within
 * TIMEOUT_NS is a hang. The number presented successfully, or -1. */
static int run_frames(Present *p, unsigned frames)
{
    unsigned done = 0;
    VkResult r;

    for (; done < frames; done++) {
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        uint32_t index = 0;
        VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &p->acquired, .pWaitDstStageMask = &stage,
                                .commandBufferCount = 1, .signalSemaphoreCount = 1 };
        VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                     .swapchainCount = 1, .pSwapchains = &p->swapchain, .pImageIndices = &index };

        r = vkAcquireNextImageKHR(device, p->swapchain, TIMEOUT_NS, p->acquired, VK_NULL_HANDLE, &index);
        if (r == VK_TIMEOUT || r == VK_NOT_READY) { p->hang = 1; failed(p, "vkAcquireNextImageKHR", r); break; }
        if (r < 0 || index >= p->image_count) { failed(p, "vkAcquireNextImageKHR", r); break; }
        submit.pCommandBuffers = &p->commands[index];
        submit.pSignalSemaphores = &p->rendered[index];
        if ((r = vkQueueSubmit(p->queue, 1, &submit, p->fence))) { failed(p, "vkQueueSubmit", r); break; }
        present.pWaitSemaphores = &p->rendered[index];
        if ((r = vkQueuePresentKHR(p->queue, &present)) < 0) { failed(p, "vkQueuePresentKHR", r); break; }
        r = vkWaitForFences(device, 1, &p->fence, VK_TRUE, TIMEOUT_NS);
        if (r == VK_TIMEOUT) { p->hang = 1; failed(p, "vkWaitForFences", r); done++; break; }
        if (r || (r = vkResetFences(device, 1, &p->fence))) { failed(p, "vkWaitForFences", r); done++; break; }
    }
    return p->failed[0] && !done ? -1 : (int)done;
}

void pw_vk_present(const char *shot, char *fields, size_t size)
{
    Present p;
    struct pw_vulkan_stats stats;
    struct stat st;
    char why[96];
    unsigned long long frames = 0, reopen_frames = 0, fps10 = 0;
    int shown = -1, reopened = -1, fb;
    VkResult r;

    memset(&p, 0, sizeof(p));
    unlink(shot);
    if (make_instance(why, sizeof(why))) { snprintf(fields, size, "present=fail:%s gpu_hang=0", why); return; }
    {
        VkHeadlessSurfaceCreateInfoEXT info = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
        INSTANCE_FN(vkCreateHeadlessSurfaceEXT);
        if (!vkCreateHeadlessSurfaceEXT) failed(&p, "novkCreateHeadlessSurfaceEXT", VK_ERROR_EXTENSION_NOT_PRESENT);
        else if ((r = vkCreateHeadlessSurfaceEXT(instance, &info, NULL, &p.surface)))
            failed(&p, "vkCreateHeadlessSurfaceEXT", r);
    }
    if (!p.failed[0] && !make_device(&p)) {
        VkSurfaceFormatKHR formats[16];
        VkSurfaceCapabilitiesKHR caps;
        uint32_t count = 16;
        INSTANCE_FN(vkGetPhysicalDeviceSurfaceFormatsKHR);
        INSTANCE_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);

        if (((r = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, p.surface, &count, formats)) && r != VK_INCOMPLETE) ||
            !count)
            failed(&p, "vkGetPhysicalDeviceSurfaceFormatsKHR", r);
        else if ((r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, p.surface, &caps)))
            failed(&p, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
        else if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            failed(&p, "notransferdst", VK_ERROR_FORMAT_NOT_SUPPORTED);
        if (!p.failed[0]) {
            p.format = formats[0];
            for (uint32_t i = 0; i < count; i++)
                if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) p.format = formats[i];
            /* wsi_orbis scans the images out directly from four on (two flips pending plus one held). */
            p.image_count = caps.minImageCount > 4 ? caps.minImageCount : 4;
            if (caps.maxImageCount && p.image_count > caps.maxImageCount) p.image_count = caps.maxImageCount;
            if (p.image_count > MAX_IMAGES) p.image_count = MAX_IMAGES;
            vstep("vk_present surface formats=%u first=%d chosen=%d min_images=%u usage=0x%x", count,
                  (int)formats[0].format, (int)p.format.format, caps.minImageCount, caps.supportedUsageFlags);
        }
    }
    if (!p.failed[0]) {
        VkCommandPoolCreateInfo pool = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .queueFamilyIndex = p.family };
        VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fence = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if ((r = vkCreateCommandPool(device, &pool, NULL, &p.pool))) failed(&p, "vkCreateCommandPool", r);
        else if ((r = vkCreateSemaphore(device, &semaphore, NULL, &p.acquired))) failed(&p, "vkCreateSemaphore", r);
        else if ((r = vkCreateFence(device, &fence, NULL, &p.fence))) failed(&p, "vkCreateFence", r);
        for (unsigned i = 0; !p.failed[0] && i < MAX_IMAGES; i++)
            if ((r = vkCreateSemaphore(device, &semaphore, NULL, &p.rendered[i]))) failed(&p, "vkCreateSemaphore", r);
        if (!p.failed[0]) make_mark(&p);
    }
    /* The first swapchain: RADV's WSI opens VideoOut here. */
    if (!p.failed[0] && !make_swapchain(&p)) {
        uint64_t t0 = host.now_ns(), elapsed;
        vstep("vk_present swapchain images=%u format=%d: %u frames", p.image_count, (int)p.format.format,
              PW_VK_E1_FRAMES);
        shown = run_frames(&p, PW_VK_E1_FRAMES);
        elapsed = host.now_ns() - t0;
        if (shown > 0 && elapsed) fps10 = (unsigned long long)shown * 10000000000ull / elapsed;
    }
    pw_vk_stats(&stats);
    frames = stats.presents;
    fb = stat(shot, &st) == 0 && st.st_size == (off_t)WIDTH * HEIGHT * 4;
    vstep("vk_present first swapchain: shown=%d counted=%llu fps=%llu.%llu hang=%d failed=%s fb=%d", shown,
          (unsigned long long)stats.presents, fps10 / 10, fps10 % 10, p.hang, p.failed[0] ? p.failed : "-", fb);
    /* The second: closed and opened again, as DXVK recreates its swapchain. */
    if (!p.failed[0] && !p.hang) {
        vkDeviceWaitIdle(device);
        vkDestroySwapchainKHR(device, p.swapchain, NULL);
        p.swapchain = VK_NULL_HANDLE;
        if (!make_swapchain(&p)) reopened = run_frames(&p, PW_VK_E1_REOPEN_FRAMES);
        if (reopened > 0) reopen_frames = (unsigned long long)reopened;
        vkDeviceWaitIdle(device);
        if (p.swapchain) vkDestroySwapchainKHR(device, p.swapchain, NULL);
    }
    pw_vk_stats(&stats);
    snprintf(fields, size, "present=%s frames=%llu gpu_hang=%d swapchain=%ux%u fmt=%d images=%u fps=%llu.%llu fb=%s "
             "reopen=%s reopen_frames=%llu counted=%d device_lost=%u last_error=%d dmem_free_mib=%llu "
             "flex_free_mib=%llu",
             p.failed[0] ? p.failed : "ok", frames, p.hang || stats.device_lost, stats.width, stats.height,
             (int)p.format.format, p.image_count, fps10 / 10, fps10 % 10, fb ? "ok" : "none",
             reopened == PW_VK_E1_REOPEN_FRAMES ? "ok" : reopened < 0 ? "-" : "fail", reopen_frames,
             shown >= 0 && stats.presents == (uint64_t)shown + reopen_frames, stats.device_lost, stats.last_error,
             dmem_free_mib(), flex_free_mib());
}
