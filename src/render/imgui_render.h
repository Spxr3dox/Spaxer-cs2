#pragma once
#include <vulkan/vulkan.h>

struct ImguiInitParams {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t queueFamily;
    VkQueue queue;
    VkFormat swapFormat;
    uint32_t imageCount;
    uint32_t width;
    uint32_t height;
};

void ImguiEnsure(const ImguiInitParams& p);
void ImguiRenderFrame(VkCommandBuffer cb, VkImageView view, uint32_t w, uint32_t h);
bool ImguiReady();
