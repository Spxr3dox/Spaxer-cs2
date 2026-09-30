#include "../core/core.h"
#include "../render/imgui_render.h"
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <vector>
#include <unordered_map>

using PFN_gipa_t = PFN_vkVoidFunction (*)(VkInstance, const char*);
using PFN_gdpa_t = PFN_vkVoidFunction (*)(VkDevice, const char*);

static PFN_gipa_t g_real_gipa = nullptr;
static PFN_gdpa_t g_real_gdpa = nullptr;
static PFN_vkQueuePresentKHR g_real_present = nullptr;
static PFN_vkCreateInstance g_real_ci = nullptr;
static PFN_vkCreateSwapchainKHR g_real_create_swap = nullptr;

static VkInstance g_instance = VK_NULL_HANDLE;
static VkDevice g_device = VK_NULL_HANDLE;
static VkQueue g_queue = VK_NULL_HANDLE;
static uint32_t g_queue_family = UINT32_MAX;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;

struct DeviceFns {
    PFN_vkGetSwapchainImagesKHR getSwapImages;
    PFN_vkQueueSubmit queueSubmit;
    PFN_vkCreateCommandPool createPool;
    PFN_vkAllocateCommandBuffers allocCB;
    PFN_vkBeginCommandBuffer beginCB;
    PFN_vkEndCommandBuffer endCB;
    PFN_vkCreateFence createFence;
    PFN_vkWaitForFences waitFences;
    PFN_vkResetFences resetFences;
    PFN_vkCreateImageView createIV;
    PFN_vkDeviceWaitIdle waitIdle;
    PFN_vkCreateSemaphore createSem;
    PFN_vkDestroySemaphore destroySem;
};
static DeviceFns g_fns{};

struct SwapInfo {
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkExtent2D extent;
    VkFormat format;
    VkCommandPool pool;
    std::vector<VkCommandBuffer> cbs;
    std::vector<VkFence> fences;
    std::vector<VkSemaphore> sems;
    uint32_t imageCount;
    bool ready;
};

static std::mutex g_mtx;
static std::unordered_map<VkSwapchainKHR, SwapInfo> g_swaps;
static std::atomic<uint64_t> g_frames{0};
static std::atomic<bool> g_setup_done{false};
static std::atomic<bool> g_setup_failed{false};

static void LoadDeviceFns(VkDevice dev) {
    if (!g_real_gdpa) return;
    #define G(name) g_fns.name = (PFN_vk##name)g_real_gdpa(dev, "vk" #name)
    #define GX(store, name) g_fns.store = (PFN_vk##name)g_real_gdpa(dev, "vk" #name)
    GX(getSwapImages, GetSwapchainImagesKHR);
    GX(queueSubmit,   QueueSubmit);
    GX(createPool,    CreateCommandPool);
    GX(allocCB,       AllocateCommandBuffers);
    GX(beginCB,       BeginCommandBuffer);
    GX(endCB,         EndCommandBuffer);
    GX(createFence,   CreateFence);
    GX(waitFences,    WaitForFences);
    GX(resetFences,   ResetFences);
    GX(createIV,      CreateImageView);
    GX(waitIdle,      DeviceWaitIdle);
    GX(createSem,     CreateSemaphore);
    GX(destroySem,    DestroySemaphore);
    #undef G
    #undef GX
}

static bool SetupSwap(VkDevice dev, SwapInfo& s) {
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_queue_family;
    if (g_fns.createPool(dev, &pci, nullptr, &s.pool) != VK_SUCCESS) return false;

    s.cbs.resize(s.images.size());
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = s.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (uint32_t)s.cbs.size();
    g_fns.allocCB(dev, &ai, s.cbs.data());

    s.fences.resize(s.images.size(), VK_NULL_HANDLE);
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (auto& f : s.fences) g_fns.createFence(dev, &fci, nullptr, &f);

    s.sems.resize(s.images.size(), VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sci{}; sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto& sm : s.sems) g_fns.createSem(dev, &sci, nullptr, &sm);

    s.views.resize(s.images.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < s.images.size(); i++) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = s.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s.format;
        vi.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        g_fns.createIV(dev, &vi, nullptr, &s.views[i]);
    }
    s.ready = true;
    Log("[render] per-swap resources for %zu images", s.images.size());
    return true;
}

static bool SetupOnce(VkDevice dev, VkQueue q, VkSwapchainKHR sc, SwapInfo& s) {
    if (g_setup_done.load()) return true;
    if (g_setup_failed.load()) return false;
    if (!g_real_gipa) g_real_gipa = (PFN_gipa_t)dlsym(RTLD_NEXT, "vkGetInstanceProcAddr");
    if (!g_real_gdpa) g_real_gdpa = (PFN_gdpa_t)dlsym(RTLD_NEXT, "vkGetDeviceProcAddr");
    if (!g_real_gipa || !g_real_gdpa) { static bool o=false; if(!o){o=true; Log("[setup] no gipa=%p gdpa=%p", (void*)g_real_gipa, (void*)g_real_gdpa);} return false; }

    LoadDeviceFns(dev);
    if (!g_fns.createPool || !g_fns.allocCB || !g_fns.createFence || !g_fns.createIV) {
        Log("[render] device fns missing"); g_setup_failed = true; return false;
    }

    auto enumPhys = (PFN_vkEnumeratePhysicalDevices)g_real_gipa(g_instance, "vkEnumeratePhysicalDevices");
    auto getQFP = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)g_real_gipa(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!enumPhys || !getQFP) { Log("[setup] no enumPhys/getQFP"); g_setup_failed = true; return false; }

    uint32_t cnt = 0; enumPhys(g_instance, &cnt, nullptr);
    if (!cnt) { Log("[setup] no phys devices"); g_setup_failed = true; return false; }
    std::vector<VkPhysicalDevice> devs(cnt);
    enumPhys(g_instance, &cnt, devs.data());
    g_phys = devs[0];

    uint32_t qcnt = 0; getQFP(g_phys, &qcnt, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qcnt);
    getQFP(g_phys, &qcnt, qprops.data());
    for (uint32_t i = 0; i < qcnt; i++) {
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_queue_family = i; break; }
    }
    if (g_queue_family == UINT32_MAX) g_queue_family = 0;

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_queue_family;
    Log("[setup] creating pool qf=%u", g_queue_family); if (g_fns.createPool(dev, &pci, nullptr, &s.pool) != VK_SUCCESS) { Log("[setup] pool fail"); g_setup_failed = true; return false; }

    s.cbs.resize(s.images.size());
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = s.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (uint32_t)s.cbs.size();
    g_fns.allocCB(dev, &ai, s.cbs.data());

    s.fences.resize(s.images.size(), VK_NULL_HANDLE);
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (auto& f : s.fences) g_fns.createFence(dev, &fci, nullptr, &f);

    ImguiInitParams p{};
    p.instance = g_instance;
    p.phys = g_phys;
    p.device = dev;
    p.queueFamily = g_queue_family;
    p.queue = q;
    p.swapFormat = s.format;
    p.imageCount = s.imageCount;
    p.width = s.extent.width;
    p.height = s.extent.height;
    ImguiEnsure(p);
    if (!ImguiReady()) { Log("[render] imgui init failed"); g_setup_failed = true; return false; }

    g_setup_done = true;
    Log("[render] setup done: dev=%p q=%p qf=%u phys=%p", (void*)dev, (void*)q, g_queue_family, (void*)g_phys);
    return SetupSwap(dev, s);
}

static VkResult VKAPI_CALL Hook_QueuePresent(VkQueue q, const VkPresentInfoKHR* info) {
    uint64_t n = g_frames.fetch_add(1) + 1;
    if ((n % 240) == 0) Log("[render] frame %lu", (unsigned long)n);

    if (g_queue == VK_NULL_HANDLE) g_queue = q;

    if (g_device != VK_NULL_HANDLE && g_instance != VK_NULL_HANDLE && info && info->swapchainCount > 0 && !g_setup_failed.load()) {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (uint32_t i = 0; i < info->swapchainCount; i++) {
            auto it = g_swaps.find(info->pSwapchains[i]);
            if (it == g_swaps.end()) { static bool once=false; if(!once){once=true; Log("[render] swap %p not in map (have %zu)", (void*)info->pSwapchains[i], g_swaps.size());} continue; }
            SwapInfo& s = it->second;
            if (!s.ready) {
                if (!g_setup_done.load()) { if (!SetupOnce(g_device, q, info->pSwapchains[i], s)) continue; }
                else if (!SetupSwap(g_device, s)) continue;
            }
            if (!s.ready) continue;

            uint32_t idx = info->pImageIndices[i];
            if (idx >= s.cbs.size()) continue;
            g_fns.waitFences(g_device, 1, &s.fences[idx], VK_TRUE, UINT64_MAX);
            g_fns.resetFences(g_device, 1, &s.fences[idx]);

            VkCommandBuffer cb = s.cbs[idx];
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            g_fns.beginCB(cb, &bi);
            ImguiRenderFrame(cb, s.views[idx], s.extent.width, s.extent.height);
            g_fns.endCB(cb);

            std::vector<VkPipelineStageFlags> waitStages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.waitSemaphoreCount = info->waitSemaphoreCount;
            si.pWaitSemaphores = info->pWaitSemaphores;
            si.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &s.sems[idx];
            g_fns.queueSubmit(g_queue, 1, &si, s.fences[idx]);

            VkPresentInfoKHR pi = *info;
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &s.sems[idx];
            return g_real_present(q, &pi);
        }
    }
    return g_real_present(q, info);
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    VkResult r = g_real_create_swap(dev, ci, alloc, out);
    if (r != VK_SUCCESS) return r;
    if (!g_real_gdpa) g_real_gdpa = (PFN_gdpa_t)dlsym(RTLD_NEXT, "vkGetDeviceProcAddr");
    LoadDeviceFns(dev);
    if (!g_fns.getSwapImages) return r;
    uint32_t cnt = 0;
    g_fns.getSwapImages(dev, *out, &cnt, nullptr);
    std::vector<VkImage> imgs(cnt);
    g_fns.getSwapImages(dev, *out, &cnt, imgs.data());

    std::lock_guard<std::mutex> lk(g_mtx);
    g_device = dev;
    SwapInfo s{};
    s.images = imgs;
    s.extent = ci->imageExtent;
    s.format = ci->imageFormat;
    s.imageCount = cnt;
    g_swaps[*out] = std::move(s);
    Log("[render] swapchain %p: %ux%u fmt=%d images=%u", (void*)*out, ci->imageExtent.width, ci->imageExtent.height, ci->imageFormat, cnt);
    return r;
}

static PFN_vkVoidFunction Dispatch(const char* name, PFN_vkVoidFunction real) {
    if (!name || !real) return real;
    if (!std::strcmp(name, "vkQueuePresentKHR")) {
        if (!g_real_present) g_real_present = (PFN_vkQueuePresentKHR)real;
        return (PFN_vkVoidFunction)Hook_QueuePresent;
    }
    if (!std::strcmp(name, "vkCreateSwapchainKHR")) {
        if (!g_real_create_swap) g_real_create_swap = (PFN_vkCreateSwapchainKHR)real;
        return (PFN_vkVoidFunction)Hook_CreateSwapchain;
    }
    return real;
}

extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance inst, const char* name) {
    if (!g_real_gipa) {
        g_real_gipa = (PFN_gipa_t)dlsym(RTLD_NEXT, "vkGetInstanceProcAddr");
        if (!g_real_gipa) return nullptr;
    }
    if (inst != VK_NULL_HANDLE && g_instance == VK_NULL_HANDLE) {
        g_instance = inst;
        Log("[render] instance captured via gipa %p", (void*)inst);
    }
    return Dispatch(name, g_real_gipa(inst, name));
}

extern "C" PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice dev, const char* name) {
    if (!g_real_gdpa) {
        g_real_gdpa = (PFN_gdpa_t)dlsym(RTLD_NEXT, "vkGetDeviceProcAddr");
        if (!g_real_gdpa) return nullptr;
    }
    return Dispatch(name, g_real_gdpa(dev, name));
}

extern "C" VkResult vkCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    if (!g_real_ci) g_real_ci = (PFN_vkCreateInstance)dlsym(RTLD_NEXT, "vkCreateInstance");
    if (!g_real_ci) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = g_real_ci(ci, a, out);
    if (r == VK_SUCCESS) { g_instance = *out; Log("[render] instance %p", (void*)*out); }
    return r;
}

uint64_t PresentFrameCount() { return g_frames.load(); }
