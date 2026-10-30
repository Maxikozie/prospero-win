/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A 64-bit Vulkan program for the PS4 port's milestones e2 and e3: Wine's
 * Vulkan (vulkan-1.dll, winevulkan and its Unix side, win32u's Vulkan
 * driver, the GPU driver behind them) without DXVK. Two lines on stdout,
 * the first before anything presents, so a failure in the second leaves it:
 *
 *   VK64 inst=ok api=1.3 device=NAME exts=N exts_dxvk=ok|MISSING dev=ok submit=ok
 *   VK64S surface=ok swapchain=WxH fmt=N images=N frames=N hang=0|1 present=ok|CALL:RESULT
 *
 * e2: an instance with VK_KHR_win32_surface, the first device, the device
 * extensions DXVK 2.6 needs as the application sees them (winevulkan may
 * hide some), a device and one empty submit waited on with a fence.
 * e3: a 1920x1080 popup window, its win32 surface (win32u's driver makes a
 * headless one on the PS4, patch 0461, after the title hands the video
 * output over), a swapchain of its size, 330 frames (RADV defers each present
 * by one; the shot knob takes the 300th) cleared to red with a
 * 64x64 green square on 240,135 copied in as bytes, so swapped red and blue
 * fail both. A fence or an acquire later than 2 s is a hang.
 *
 * Exit 0 when both lines say ok. Built with x86_64-w64-mingw32-gcc against
 * the Khronos headers (-DVK_USE_PLATFORM_WIN32_KHR); vulkan-1.dll is loaded
 * at run time. tests/test_wine_vk64.py runs it under the host Wine. */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

enum { WIDTH = 1920, HEIGHT = 1080, FRAMES = 330, MARK = 64, MARK_X = 240, MARK_Y = 135, MAX_IMAGES = 8 };
static const UINT64 TIMEOUT_NS = 2000000000ull;

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static uint32_t family;
static char failed[96];

#define INSTANCE_FN(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
#define DEVICE_FN(name) PFN_##name name = (PFN_##name)gdpa(device, #name)

static int fail(const char *call, VkResult r)
{
    if (!failed[0]) snprintf(failed, sizeof(failed), "%s:%d", call, (int)r);
    return -1;
}

/* deviceName with anything but [A-Za-z0-9.-] as '_'. */
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

/* e2: the instance, the device, DXVK's extensions, one submit. */
static int phase1(void)
{
    static const char *const instance_exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    static const char *const dxvk_exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME,
        VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
    };
    static VkExtensionProperties exts[512];
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk64",
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                  .enabledExtensionCount = 2, .ppEnabledExtensionNames = instance_exts };
    VkPhysicalDeviceProperties props;
    VkQueueFamilyProperties families[16];
    uint32_t count = 1, ext_count = 512, family_count = 16;
    const char *missing = NULL;
    char name[96];
    VkResult r;
    HMODULE vulkan = LoadLibraryA("vulkan-1.dll");

    if (!vulkan || !(gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(vulkan, "vkGetInstanceProcAddr"))) {
        printf("VK64 inst=fail:vulkan-1.dll:%lu\n", GetLastError());
        return -1;
    }
    {
        PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
        if ((r = vkCreateInstance(&info, NULL, &instance))) {
            printf("VK64 inst=fail:vkCreateInstance:%d\n", (int)r);
            return -1;
        }
    }
    {
        INSTANCE_FN(vkEnumeratePhysicalDevices);
        INSTANCE_FN(vkGetPhysicalDeviceProperties);
        INSTANCE_FN(vkEnumerateDeviceExtensionProperties);
        INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
        r = vkEnumeratePhysicalDevices(instance, &count, &physical);
        if ((r && r != VK_INCOMPLETE) || !count) {
            printf("VK64 inst=ok device=none:%d:%u\n", (int)r, count);
            return -1;
        }
        vkGetPhysicalDeviceProperties(physical, &props);
        vkEnumerateDeviceExtensionProperties(physical, NULL, &ext_count, exts);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    }
    for (size_t i = 0; i < sizeof(dxvk_exts) / sizeof(dxvk_exts[0]) && !missing; i++) {
        uint32_t j;
        for (j = 0; j < ext_count && strcmp(exts[j].extensionName, dxvk_exts[i]); j++) {}
        if (j == ext_count) missing = dxvk_exts[i];
    }
    for (family = 0; family < family_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT); family++) {}
    field_text(props.deviceName, name, sizeof(name));
    printf("VK64 inst=ok api=%u.%u device=%s exts=%u exts_dxvk=%s ", VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), name, ext_count, missing ? missing : "ok");
    {
        static const char *const device_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                               .queueFamilyIndex = family, .queueCount = 1,
                                               .pQueuePriorities = &priority };
        VkDeviceCreateInfo device_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                           .pQueueCreateInfos = &queue_info, .enabledExtensionCount = 1,
                                           .ppEnabledExtensionNames = device_exts };
        INSTANCE_FN(vkCreateDevice);
        gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
        if (family == family_count) r = VK_ERROR_FEATURE_NOT_PRESENT;
        else r = vkCreateDevice(physical, &device_info, NULL, &device);
        if (r) {
            printf("dev=fail:%d submit=-\n", (int)r);
            return -1;
        }
        {
            VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkFence fence;
            DEVICE_FN(vkGetDeviceQueue);
            DEVICE_FN(vkCreateFence);
            DEVICE_FN(vkQueueSubmit);
            DEVICE_FN(vkWaitForFences);
            DEVICE_FN(vkDestroyFence);
            vkGetDeviceQueue(device, family, 0, &queue);
            if (!(r = vkCreateFence(device, &fence_info, NULL, &fence))) {
                if (!(r = vkQueueSubmit(queue, 0, NULL, fence))) r = vkWaitForFences(device, 1, &fence, VK_TRUE, TIMEOUT_NS);
                vkDestroyFence(device, fence, NULL);
            }
        }
    }
    if (r) printf("dev=ok submit=fail:%d\n", (int)r);
    else printf("dev=ok submit=ok\n");
    fflush(stdout);
    return (r || missing) ? -1 : 0;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

typedef struct Frames {
    VkSwapchainKHR swapchain;
    VkImage images[MAX_IMAGES];
    VkCommandBuffer commands[MAX_IMAGES];
    VkSemaphore acquired, rendered[MAX_IMAGES];
    VkFence fence;
    VkBuffer mark;
    uint32_t count;
} Frames;

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                    VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src,
                               .dstAccessMask = dst, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                               .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    DEVICE_FN(vkCmdPipelineBarrier);
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* The green square's bytes (R,G,B,A) in a host-visible buffer. */
static int make_mark(Frames *f)
{
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MARK * MARK * 4,
                                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VkPhysicalDeviceMemoryProperties memory;
    VkMemoryRequirements needs;
    VkDeviceMemory backing;
    VkResult r;
    UINT8 *bytes;
    uint32_t type;
    INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
    DEVICE_FN(vkCreateBuffer);
    DEVICE_FN(vkGetBufferMemoryRequirements);
    DEVICE_FN(vkAllocateMemory);
    DEVICE_FN(vkBindBufferMemory);
    DEVICE_FN(vkMapMemory);

    if ((r = vkCreateBuffer(device, &info, NULL, &f->mark))) return fail("vkCreateBuffer", r);
    vkGetBufferMemoryRequirements(device, f->mark, &needs);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (type = 0; type < memory.memoryTypeCount; type++) {
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((needs.memoryTypeBits & (1u << type)) && (memory.memoryTypes[type].propertyFlags & want) == want) break;
    }
    if (type == memory.memoryTypeCount) return fail("nohostmemory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    {
        VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                          .allocationSize = needs.size, .memoryTypeIndex = type };
        if ((r = vkAllocateMemory(device, &allocate, NULL, &backing))) return fail("vkAllocateMemory", r);
    }
    if ((r = vkBindBufferMemory(device, f->mark, backing, 0))) return fail("vkBindBufferMemory", r);
    if ((r = vkMapMemory(device, backing, 0, VK_WHOLE_SIZE, 0, (void **)&bytes))) return fail("vkMapMemory", r);
    for (unsigned i = 0; i < MARK * MARK; i++) {
        bytes[i * 4] = 0;
        bytes[i * 4 + 1] = 255;
        bytes[i * 4 + 2] = 0;
        bytes[i * 4 + 3] = 255;
    }
    return 0;
}

/* e3: the window's surface, its swapchain and FRAMES frames. */
static int phase2(void)
{
    WNDCLASSW window_class = { .lpfnWndProc = window_proc, .hInstance = GetModuleHandleW(NULL),
                               .lpszClassName = L"vk64" };
    VkWin32SurfaceCreateInfoKHR surface_info = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    VkSurfaceFormatKHR formats[16], format;
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceKHR surface;
    VkExtent2D extent = { WIDTH, HEIGHT };
    Frames f;
    uint32_t count = 16, frames = 0;
    int hang = 0;
    VkResult r;
    HWND window;
    INSTANCE_FN(vkCreateWin32SurfaceKHR);
    INSTANCE_FN(vkGetPhysicalDeviceSurfaceFormatsKHR);
    INSTANCE_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    INSTANCE_FN(vkGetPhysicalDeviceSurfaceSupportKHR);
    DEVICE_FN(vkCreateSwapchainKHR);
    DEVICE_FN(vkGetSwapchainImagesKHR);
    DEVICE_FN(vkAcquireNextImageKHR);
    DEVICE_FN(vkQueuePresentKHR);
    DEVICE_FN(vkQueueSubmit);
    DEVICE_FN(vkWaitForFences);
    DEVICE_FN(vkResetFences);
    DEVICE_FN(vkCreateCommandPool);
    DEVICE_FN(vkAllocateCommandBuffers);
    DEVICE_FN(vkBeginCommandBuffer);
    DEVICE_FN(vkEndCommandBuffer);
    DEVICE_FN(vkCmdClearColorImage);
    DEVICE_FN(vkCmdCopyBufferToImage);
    DEVICE_FN(vkCreateSemaphore);
    DEVICE_FN(vkCreateFence);
    DEVICE_FN(vkDeviceWaitIdle);
    DEVICE_FN(vkDestroySwapchainKHR);
    INSTANCE_FN(vkDestroySurfaceKHR);

    memset(&f, 0, sizeof(f));
    RegisterClassW(&window_class);
    window = CreateWindowExW(0, L"vk64", L"vk64", WS_POPUP | WS_VISIBLE, 0, 0, WIDTH, HEIGHT, NULL, NULL,
                             window_class.hInstance, NULL);
    if (!window) { printf("VK64S surface=fail:window:%lu\n", GetLastError()); return -1; }
    surface_info.hinstance = window_class.hInstance;
    surface_info.hwnd = window;
    if ((r = vkCreateWin32SurfaceKHR(instance, &surface_info, NULL, &surface))) {
        printf("VK64S surface=fail:vkCreateWin32SurfaceKHR:%d\n", (int)r);
        return -1;
    }
    {
        VkBool32 supported = VK_FALSE;
        if ((r = vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &supported)) || !supported)
            fail("vkGetPhysicalDeviceSurfaceSupportKHR", r ? r : VK_ERROR_SURFACE_LOST_KHR);
    }
    if (!failed[0] && (((r = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats)) &&
                        r != VK_INCOMPLETE) || !count))
        fail("vkGetPhysicalDeviceSurfaceFormatsKHR", r);
    if (!failed[0] && (r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps)))
        fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
    format = formats[0];
    if (!failed[0]) {
        for (uint32_t i = 0; i < count; i++)
            if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) format = formats[i];
        /* The window's size where the surface reports one (win32u: its client area). */
        if (caps.currentExtent.width != 0xffffffffu) extent = caps.currentExtent;
        f.count = caps.minImageCount > 4 ? caps.minImageCount : 4;
        if (caps.maxImageCount && f.count > caps.maxImageCount) f.count = caps.maxImageCount;
        if (f.count > MAX_IMAGES) f.count = MAX_IMAGES;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            fail("notransferdst", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    if (!failed[0]) {
        VkSwapchainCreateInfoKHR info = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = f.count,
            .imageFormat = format.format, .imageColorSpace = format.colorSpace, .imageExtent = extent,
            .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
        count = MAX_IMAGES;
        if ((r = vkCreateSwapchainKHR(device, &info, NULL, &f.swapchain))) fail("vkCreateSwapchainKHR", r);
        else if ((r = vkGetSwapchainImagesKHR(device, f.swapchain, &count, f.images)) && r != VK_INCOMPLETE)
            fail("vkGetSwapchainImagesKHR", r);
        f.count = count;
    }
    if (!failed[0]) {
        VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                              .queueFamilyIndex = family };
        VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fence = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkCommandBufferAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                 .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = f.count };
        const VkClearColorValue red = { .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } };
        const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        const VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                           .imageOffset = { MARK_X - MARK / 2, MARK_Y - MARK / 2, 0 },
                                           .imageExtent = { MARK, MARK, 1 } };
        VkCommandPool pool;

        if ((r = vkCreateCommandPool(device, &pool_info, NULL, &pool))) fail("vkCreateCommandPool", r);
        else if ((r = vkCreateSemaphore(device, &semaphore, NULL, &f.acquired))) fail("vkCreateSemaphore", r);
        else if ((r = vkCreateFence(device, &fence, NULL, &f.fence))) fail("vkCreateFence", r);
        for (uint32_t i = 0; !failed[0] && i < f.count; i++)
            if ((r = vkCreateSemaphore(device, &semaphore, NULL, &f.rendered[i]))) fail("vkCreateSemaphore", r);
        if (!failed[0]) make_mark(&f);
        allocate.commandPool = pool;
        if (!failed[0] && (r = vkAllocateCommandBuffers(device, &allocate, f.commands)))
            fail("vkAllocateCommandBuffers", r);
        for (uint32_t i = 0; !failed[0] && i < f.count; i++) {
            VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            VkCommandBuffer cmd = f.commands[i];
            vkBeginCommandBuffer(cmd, &begin);
            barrier(cmd, f.images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdClearColorImage(cmd, f.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
            barrier(cmd, f.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdCopyBufferToImage(cmd, f.mark, f.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier(cmd, f.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            if ((r = vkEndCommandBuffer(cmd))) fail("vkEndCommandBuffer", r);
        }
    }
    for (; !failed[0] && frames < FRAMES; frames++) {
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        uint32_t index = 0;
        VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &f.acquired, .pWaitDstStageMask = &stage,
                                .commandBufferCount = 1, .signalSemaphoreCount = 1 };
        VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                     .swapchainCount = 1, .pSwapchains = &f.swapchain, .pImageIndices = &index };
        MSG message;

        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        r = vkAcquireNextImageKHR(device, f.swapchain, TIMEOUT_NS, f.acquired, VK_NULL_HANDLE, &index);
        if (r == VK_TIMEOUT || r == VK_NOT_READY) { hang = 1; fail("vkAcquireNextImageKHR", r); break; }
        if (r < 0 || index >= f.count) { fail("vkAcquireNextImageKHR", r); break; }
        submit.pCommandBuffers = &f.commands[index];
        submit.pSignalSemaphores = &f.rendered[index];
        if ((r = vkQueueSubmit(queue, 1, &submit, f.fence))) { fail("vkQueueSubmit", r); break; }
        present.pWaitSemaphores = &f.rendered[index];
        if ((r = vkQueuePresentKHR(queue, &present)) < 0) { fail("vkQueuePresentKHR", r); break; }
        r = vkWaitForFences(device, 1, &f.fence, VK_TRUE, TIMEOUT_NS);
        if (r == VK_TIMEOUT) { hang = 1; fail("vkWaitForFences", r); frames++; break; }
        if (r || (r = vkResetFences(device, 1, &f.fence))) { fail("vkWaitForFences", r); frames++; break; }
    }
    printf("VK64S surface=ok swapchain=%ux%u fmt=%d images=%u frames=%u hang=%d present=%s\n", extent.width,
           extent.height, (int)format.format, f.count, frames, hang, failed[0] ? failed : "ok");
    fflush(stdout);
    vkDeviceWaitIdle(device);
    if (f.swapchain) vkDestroySwapchainKHR(device, f.swapchain, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    DestroyWindow(window);
    return failed[0] ? -1 : 0;
}

int main(void)
{
    int ok = phase1() == 0;
    if (!device) return 1;
    return phase2() == 0 && ok ? 0 : 1;
}
