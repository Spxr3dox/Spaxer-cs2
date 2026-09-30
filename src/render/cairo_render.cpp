#include "cairo_render.h"
#include "../core/core.h"
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <string>
#include <atomic>

static cairo_surface_t* g_surf = nullptr;
static cairo_t* g_cr = nullptr;
static uint32_t g_w = 0, g_h = 0;
static PangoFontDescription* g_font_reg = nullptr;
static PangoFontDescription* g_font_bold = nullptr;

extern "C" bool  SpaxerKey(int k);

void CairoInit(uint32_t w, uint32_t h) {
    if (g_surf && w == g_w && h == g_h) return;
    if (g_surf) cairo_surface_destroy(g_surf);
    g_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    g_w = w; g_h = h;
    if (!g_font_reg) {
        g_font_reg  = pango_font_description_from_string("Cantarell 11");
        g_font_bold = pango_font_description_from_string("Cantarell Bold 11");
    }
    Log("[cairo] surface %ux%u", w, h);
}

void CairoBeginFrame(uint32_t w, uint32_t h) {
    CairoInit(w, h);
    if (g_cr) cairo_destroy(g_cr);
    g_cr = cairo_create(g_surf);
    cairo_set_operator(g_cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(g_cr);
    cairo_set_operator(g_cr, CAIRO_OPERATOR_OVER);
    cairo_set_antialias(g_cr, CAIRO_ANTIALIAS_BEST);
}

static void RoundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
    const double pi = 3.14159265358979;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -pi / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, pi / 2);
    cairo_arc(cr, x + r, y + h - r, r, pi / 2, pi);
    cairo_arc(cr, x + r, y + r, r, pi, 3 * pi / 2);
    cairo_close_path(cr);
}

static double TextWidth(cairo_t* cr, const char* text, bool bold) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, bold ? g_font_bold : g_font_reg);
    pango_layout_set_text(layout, text, -1);
    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    g_object_unref(layout);
    return w;
}

static void ShowText(cairo_t* cr, double x, double cy, const char* text, bool bold, double r, double g, double b, double a) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, bold ? g_font_bold : g_font_reg);
    pango_layout_set_text(layout, text, -1);
    int lw, lh;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x, cy - lh / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static void HudGlass(cairo_t* cr, double x, double y, double w, double h, double radius, double alpha) {
    for (int i = 3; i >= 1; i--) {
        RoundedRect(cr, x - i, y - i + 1, w + i * 2, h + i * 2, radius + i);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.05 * alpha);
        cairo_fill(cr);
    }
    cairo_pattern_t* fill = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgba(fill, 0.0, 0.07, 0.08, 0.11, 0.85 * alpha);
    cairo_pattern_add_color_stop_rgba(fill, 1.0, 0.03, 0.035, 0.05, 0.88 * alpha);
    RoundedRect(cr, x, y, w, h, radius);
    cairo_set_source(cr, fill);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);
    cairo_pattern_t* rim = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgba(rim, 0.0, 1, 1, 1, 0.12 * alpha);
    cairo_pattern_add_color_stop_rgba(rim, 1.0, 1, 1, 1, 0.03 * alpha);
    RoundedRect(cr, x + 0.5, y + 0.5, w - 1, h - 1, radius);
    cairo_set_source(cr, rim);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_pattern_destroy(rim);
}

static const double kAccentR = 0.30, kAccentG = 0.55, kAccentB = 1.0;

enum class Icon { Fps, Ping, Clock, User };

static void DrawIcon(cairo_t* cr, Icon icon, double cx, double cy, double s) {
    const double pi = 3.14159265358979;
    cairo_save(cr);
    cairo_set_source_rgba(cr, kAccentR, kAccentG, kAccentB, 1.0);
    cairo_set_line_width(cr, fmax(1.2, s * 0.13));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    double h = s / 2;
    switch (icon) {
        case Icon::Fps:
            for (int i = 0; i < 3; i++) {
                double bx = cx - h + i * s * 0.38, bh = s * (0.4 + i * 0.3);
                cairo_rectangle(cr, bx, cy + h - bh, s * 0.24, bh);
            }
            cairo_fill(cr); break;
        case Icon::Ping:
            for (int i = 0; i < 4; i++) {
                double bx = cx - h + i * s * 0.28, bh = s * (0.25 + i * 0.25);
                cairo_rectangle(cr, bx, cy + h - bh, s * 0.16, bh);
            }
            cairo_fill(cr); break;
        case Icon::Clock:
            cairo_arc(cr, cx, cy, h, 0, 2 * pi); cairo_stroke(cr);
            cairo_move_to(cr, cx, cy - h * 0.55); cairo_line_to(cr, cx, cy);
            cairo_line_to(cr, cx + h * 0.45, cy + h * 0.2); cairo_stroke(cr);
            break;
        case Icon::User:
            cairo_arc(cr, cx, cy - h * 0.35, h * 0.42, 0, 2 * pi); cairo_fill(cr);
            cairo_arc(cr, cx, cy + h * 1.05, h * 0.85, pi, 2 * pi); cairo_fill(cr);
            break;
    }
    cairo_restore(cr);
}

static void DrawWatermarkCairo(cairo_t* cr, double W, double H, double fps) {
    time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt);
    char timebuf[16]; snprintf(timebuf, sizeof(timebuf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    static std::string user;
    if (user.empty()) { const char* u = getenv("USER"); user = u ? u : "player"; }
    char sfps[16], sms[16];
    snprintf(sfps, sizeof(sfps), "%.0f FPS", fps);
    snprintf(sms,  sizeof(sms),  "0 MS");

    struct Seg { Icon icon; const char* text; };
    Seg segs[] = { {Icon::Fps, sfps}, {Icon::Ping, sms}, {Icon::Clock, timebuf}, {Icon::User, user.c_str()} };
    double font_size = 11, icon = 10, gap = 5, spacing = 12, pad = 10, h = 26;
    double logo_w = TextWidth(cr, "SPAXER", true);
    double total_w = pad + logo_w + spacing;
    double widths[4];
    for (int i = 0; i < 4; i++) {
        widths[i] = TextWidth(cr, segs[i].text, true);
        total_w += icon + gap + widths[i] + (i < 3 ? spacing : 0);
    }
    total_w += pad;
    double x = W - total_w - 14, y = 10, cy = y + h / 2;
    HudGlass(cr, x, y, total_w, h, 7, 1.0);
    double cx = x + pad;
    ShowText(cr, cx, cy, "SPAXER", true, kAccentR, kAccentG, kAccentB, 1.0);
    cx = x + pad + logo_w + spacing;
    for (int i = 0; i < 4; i++) {
        DrawIcon(cr, segs[i].icon, cx + icon / 2, cy, icon);
        cx += icon + gap;
        ShowText(cr, cx, cy, segs[i].text, true, 0.9, 0.91, 0.94, 1.0);
        cx += widths[i] + spacing;
    }
}

static void DrawKeystrokesCairo(cairo_t* cr, double W, double H) {
    double k = 28, gap = 4;
    double total_w = k * 3 + gap * 2;
    double cx = W * 0.5 - total_w * 0.5;
    double cy = H - 130;
    struct KB { const char* lbl; int id; double x; double y; double w; };
    KB keys[] = {
        {"W",     0, cx + k + gap,        cy,               k},
        {"A",     1, cx,                  cy + k + gap,     k},
        {"S",     2, cx + k + gap,        cy + k + gap,     k},
        {"D",     3, cx + 2*(k+gap),      cy + k + gap,     k},
        {"SPACE", 4, cx,                  cy + 2*(k+gap),   total_w},
        {"SHIFT", 5, cx + total_w + gap,  cy + k + gap,     k * 1.6},
    };
    for (auto& kk : keys) {
        bool on = SpaxerKey(kk.id) != 0;
        HudGlass(cr, kk.x, kk.y, kk.w, k, 4, 1.0);
        if (on) {
            RoundedRect(cr, kk.x, kk.y, kk.w, k, 4);
            cairo_set_source_rgba(cr, kAccentR, kAccentG, kAccentB, 0.85);
            cairo_fill(cr);
        }
        double lw = TextWidth(cr, kk.lbl, true);
        ShowText(cr, kk.x + (kk.w - lw) / 2, kk.y + k / 2, kk.lbl, true,
                 on ? 1.0 : 0.85, on ? 1.0 : 0.87, on ? 1.0 : 0.92, 1.0);
    }
}

void CairoDrawHud(float dt, float fps) {
    if (!g_cr) return;
    cairo_t* cr = g_cr;
    DrawWatermarkCairo(cr, g_w, g_h, fps);
    DrawKeystrokesCairo(cr, g_w, g_h);
}

uint8_t* CairoPixels() { return g_surf ? cairo_image_surface_get_data(g_surf) : nullptr; }
uint32_t CairoWidth()  { return g_w; }
uint32_t CairoHeight() { return g_h; }

#include "imgui.h"
#include "imgui_impl_vulkan.h"

static VkDevice   g_dev_c = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys_c = VK_NULL_HANDLE;
static VkImage       g_img = VK_NULL_HANDLE;
static VkImageView   g_view = VK_NULL_HANDLE;
static VkDeviceMemory g_mem = VK_NULL_HANDLE;
static VkSampler     g_sampler = VK_NULL_HANDLE;
static VkDescriptorSet g_ds = VK_NULL_HANDLE;
static VkBuffer      g_stage_buf = VK_NULL_HANDLE;
static VkDeviceMemory g_stage_mem = VK_NULL_HANDLE;
static void*         g_stage_ptr = nullptr;
static uint32_t      g_tex_w = 0, g_tex_h = 0;

static uint32_t FindMemType(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    return UINT32_MAX;
}

bool CairoVkSetup(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h) {
    if (g_img && w == g_tex_w && h == g_tex_h) return true;
    if (phys == VK_NULL_HANDLE) phys = g_phys_c;
    if (phys == VK_NULL_HANDLE) return false;
    g_dev_c = dev; g_phys_c = phys;
    if (g_img)     vkDestroyImage(dev, g_img, nullptr);
    if (g_view)    vkDestroyImageView(dev, g_view, nullptr);
    if (g_mem)     vkFreeMemory(dev, g_mem, nullptr);
    if (g_stage_buf) vkDestroyBuffer(dev, g_stage_buf, nullptr);
    if (g_stage_mem) vkFreeMemory(dev, g_stage_mem, nullptr);
    g_tex_w = w; g_tex_h = h;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, nullptr, &g_img) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, g_img, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = FindMemType(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &mai, nullptr, &g_mem) != VK_SUCCESS) return false;
    vkBindImageMemory(dev, g_img, g_mem, 0);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = g_img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev, &vi, nullptr, &g_view);

    if (!g_sampler) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 1.f;
        vkCreateSampler(dev, &si, nullptr, &g_sampler);
    }
    if (g_ds) ImGui_ImplVulkan_RemoveTexture(g_ds);
    g_ds = ImGui_ImplVulkan_AddTexture(g_sampler, g_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // staging buffer
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = (VkDeviceSize)w * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(dev, &bi, nullptr, &g_stage_buf);
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements(dev, g_stage_buf, &br);
    VkMemoryAllocateInfo bmi{};
    bmi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bmi.allocationSize = br.size;
    bmi.memoryTypeIndex = FindMemType(phys, br.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev, &bmi, nullptr, &g_stage_mem);
    vkBindBufferMemory(dev, g_stage_buf, g_stage_mem, 0);
    vkMapMemory(dev, g_stage_mem, 0, br.size, 0, &g_stage_ptr);

    Log("[cairo-vk] %ux%u setup done", w, h);
    return true;
}

void CairoVkUpload(VkCommandBuffer cb) {
    if (!g_img || !g_stage_ptr) return;
    size_t sz = (size_t)g_tex_w * g_tex_h * 4;
    memcpy(g_stage_ptr, CairoPixels(), sz);

    VkImageMemoryBarrier b1{};
    b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.image = g_img;
    b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b1.srcQueueFamilyIndex = b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b1);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {g_tex_w, g_tex_h, 1};
    vkCmdCopyBufferToImage(cb, g_stage_buf, g_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier b2 = b1;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b2);
}

void* CairoVkTextureID() { return g_ds; }
uint32_t CairoTexW() { return g_tex_w; }
uint32_t CairoTexH() { return g_tex_h; }
