#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

void CairoInit(uint32_t w, uint32_t h);
void CairoBeginFrame(uint32_t w, uint32_t h);
void CairoDrawHud(float dt, float fps);
uint8_t* CairoPixels();
uint32_t CairoWidth();
uint32_t CairoHeight();

bool CairoVkSetup(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h);
void CairoVkUpload(VkCommandBuffer cb);
void* CairoVkTextureID();
uint32_t CairoTexW();
uint32_t CairoTexH();
