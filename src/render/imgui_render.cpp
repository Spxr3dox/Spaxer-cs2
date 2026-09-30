#include "imgui_render.h"
#include "cairo_render.h"
#include "../core/core.h"
#include "../features/esp_internal.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <functional>

extern "C" bool  SpaxerMenuOpen();
extern "C" bool  SpaxerKey(int k);
extern "C" float SpaxerMouseX();
extern "C" float SpaxerMouseY();
extern "C" bool  SpaxerMouseL();
extern "C" bool  SpaxerMouseR();
extern "C" float SpaxerScrollTake();
extern "C" void SpxFISilentAim(int);
extern "C" void SpxFIThirdperson(int);
extern "C" void SpxFINightMode(int);
extern "C" void SpxFIAntiAim(int);


std::atomic<float> g_display_w{1920.f}, g_display_h{1080.f};

static std::mutex g_mtx;
static std::atomic<bool> g_ready{false};
static ImGuiContext* g_ctx = nullptr;
static VkDevice g_dev = VK_NULL_HANDLE;
static VkDescriptorPool g_pool = VK_NULL_HANDLE;
static VkRenderPass g_rp = VK_NULL_HANDLE;

// Exact palette from src/gui/main.cpp CSS
namespace C {
    static ImU32 accent   = IM_COL32(0x4c, 0x8d, 0xff, 255);
    static ImU32 accent2  = IM_COL32(0x2f, 0x6f, 0xe0, 255);
    static ImU32 root_bg  = IM_COL32(0x0b, 0x0c, 0x11, 255);
    static ImU32 title_bg = IM_COL32(0x0e, 0x0f, 0x15, 255);
    static ImU32 card_bg  = IM_COL32(0x11, 0x13, 0x1a, 255);
    static ImU32 field_bg = IM_COL32(0x15, 0x18, 0x23, 255);
    static ImU32 field_bd = IM_COL32(0x26, 0x2a, 0x3a, 255);
    static ImU32 root_bd  = IM_COL32(0x1d, 0x20, 0x29, 255);
    static ImU32 soft_bd  = IM_COL32(0x1a, 0x1c, 0x25, 255);
    static ImU32 card_bd  = IM_COL32(0x1c, 0x1f, 0x29, 255);
    static ImU32 text     = IM_COL32(0xe4, 0xe6, 0xed, 255);
    static ImU32 text_row = IM_COL32(0xc9, 0xcc, 0xd6, 255);
    static ImU32 text_dim = IM_COL32(0x7a, 0x7f, 0x8e, 255);
    static ImU32 text_side= IM_COL32(0x9a, 0x9e, 0xab, 255);
    static ImU32 sec_head = IM_COL32(0x4a, 0x4e, 0x5c, 255);
    static ImU32 toggle_off_bg = IM_COL32(0x1d, 0x20, 0x30, 255);
    static ImU32 toggle_off_bd = IM_COL32(0x2c, 0x31, 0x42, 255);
    static ImU32 toggle_off_kb = IM_COL32(0x8a, 0x8f, 0xa0, 255);
    static ImU32 slider_bg = IM_COL32(0x1d, 0x20, 0x30, 255);
    static ImU32 knob_white= IM_COL32(0xff, 0xff, 0xff, 255);
    static ImU32 hover     = IM_COL32(0x14, 0x16, 0x20, 255);
    static ImU32 active_grad_a = IM_COL32(0x4c, 0x8d, 0xff, 40);
    static ImU32 active_grad_b = IM_COL32(0x4c, 0x8d, 0xff, 5);
}

bool ImguiReady() { return g_ready.load(); }

void ImguiEnsure(const ImguiInitParams& p) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_ready.load()) return;

    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16},
    };
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 64;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(p.device, &dpi, nullptr, &g_pool) != VK_SUCCESS) return;

    VkAttachmentDescription att{};
    att.format = p.swapFormat;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    if (vkCreateRenderPass(p.device, &rpi, nullptr, &g_rp) != VK_SUCCESS) return;

    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)p.width, (float)p.height);
    io.IniFilename = nullptr;
    io.MouseDrawCursor = false;
    ImGui::StyleColorsDark();

    // Load Cantarell/Adwaita (GNOME/external default) for authentic look
    const char* fonts[] = {
        "/usr/share/fonts/cantarell/Cantarell-VF.otf",
        "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        nullptr
    };
    ImFontConfig cfg;
    cfg.OversampleH = 3; cfg.OversampleV = 3;
    cfg.PixelSnapH = false;
    cfg.RasterizerMultiply = 1.08f;
    for (int i = 0; fonts[i]; i++) {
        FILE* f = fopen(fonts[i], "rb"); if (!f) continue;
        fclose(f);
        if (io.Fonts->AddFontFromFileTTF(fonts[i], 15.f, &cfg)) {
            Log("[imgui] font loaded: %s", fonts[i]);
            break;
        }
    }

    static VkInstance s_inst = VK_NULL_HANDLE;
    s_inst = p.instance;
    ImGui_ImplVulkan_LoadFunctions([](const char* name, void*) -> PFN_vkVoidFunction {
        return vkGetInstanceProcAddr(s_inst, name);
    });

    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = p.instance;
    ii.PhysicalDevice = p.phys;
    ii.Device = p.device;
    ii.QueueFamily = p.queueFamily;
    ii.Queue = p.queue;
    ii.DescriptorPool = g_pool;
    ii.RenderPass = g_rp;
    ii.MinImageCount = p.imageCount;
    ii.ImageCount = p.imageCount;
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&ii)) return;

    g_dev = p.device;
    g_ready.store(true);
    CairoVkSetup(p.device, p.phys, p.width, p.height);
    Log("[imgui] initialized %ux%u", p.width, p.height);
}

static float g_fps_shown = 60.f;
static void UpdateFps(float dt) {
    static float acc = 0.f; static int n = 0;
    acc += dt; n++;
    if (acc >= 0.4f) { g_fps_shown = n / acc; acc = 0.f; n = 0; }
}

static std::string GetUser() {
    const char* u = getenv("USER"); if (u && *u) return u;
    return "player";
}

// ---------- HUD helpers ----------

static void GlassPanel(ImDrawList* dl, ImVec2 tl, ImVec2 br, float radius=6.f, float alpha=1.f) {
    // wide soft shadow (12px blur radius look)
    for (int i = 14; i >= 1; i--) {
        int a = (int)(3.5f * alpha);
        dl->AddRectFilled(ImVec2(tl.x - i, tl.y - i + 3), ImVec2(br.x + i, br.y + i + 3),
                          IM_COL32(0, 0, 0, a), radius + i);
    }
    // FLAT pure dark body (like external)
    dl->AddRectFilled(tl, br, IM_COL32(7, 9, 17, (int)(255 * alpha)), radius);
    // 1px very subtle border
    dl->AddRect(tl, br, IM_COL32(20, 24, 36, (int)(200 * alpha)), radius, 0, 1.f);
}

static void SectionHeader(ImDrawList* dl, ImVec2 tl, ImVec2 br, const char* title, const char* right="") {
    // accent underline gradient
    dl->AddRectFilledMultiColor(ImVec2(tl.x + 10, tl.y + 22), ImVec2(br.x - 10, tl.y + 23),
        C::accent, IM_COL32(0x4c, 0x8d, 0xff, 0), C::accent, IM_COL32(0x4c, 0x8d, 0xff, 0));
    dl->AddText(ImVec2(tl.x + 10, tl.y + 8), C::text, title);
    if (*right) {
        ImVec2 ts = ImGui::CalcTextSize(right);
        dl->AddText(ImVec2(br.x - ts.x - 10, tl.y + 8), C::text_dim, right);
    }
}


// ---------- HUD element positions (draggable in edit mode) ----------

struct HudPos { float x, y; bool init; };
static HudPos g_pos_watermark  = {0, 0, false};
static HudPos g_pos_bomb       = {0, 0, false};
static HudPos g_pos_velocity   = {0, 0, false};
static HudPos g_pos_spectators = {0, 0, false};
static HudPos g_pos_media      = {0, 0, false};
static HudPos g_pos_keystrokes = {0, 0, false};
static HudPos g_pos_hotkeys    = {0, 0, false};
static HudPos g_pos_radar      = {0, 0, false};
static bool   g_edit_hud       = false; // toggle in Misc

static bool HudDrag(const char* id, HudPos& pos, ImVec2 tl, ImVec2 br, const ImGuiIO& io) {
    // no key required - just hover + LMB to drag
    ImVec2 mp = io.MousePos;
    bool inside = mp.x >= tl.x && mp.x <= br.x && mp.y >= tl.y && mp.y <= br.y;
    static const char* dragging_id = nullptr;
    static float grab_dx = 0, grab_dy = 0;
    if (io.MouseDown[0]) {
        if (!dragging_id && inside) {
            dragging_id = id;
            grab_dx = mp.x - pos.x;
            grab_dy = mp.y - pos.y;
        }
        if (dragging_id == id) {
            pos.x = mp.x - grab_dx;
            pos.y = mp.y - grab_dy;
            return true;
        }
    } else {
        if (dragging_id == id) dragging_id = nullptr;
    }
    // outline while editing
    if (inside || dragging_id == id) {
        ImGui::GetForegroundDrawList()->AddRect(tl, br, C::accent, 6.f, 0, 2.f);
    } else {
        ImGui::GetForegroundDrawList()->AddRect(tl, br, IM_COL32(140, 160, 200, 120), 6.f, 0, 1.f);
    }
    return false;
}


// ---------- HUD icons (accent color, line style like external) ----------

enum class HudIcon { Fps, Ping, Clock, User, Eye, Music, Bomb };

static void DrawHudIcon(ImDrawList* dl, HudIcon icon, float cx, float cy, float sz, ImU32 c) {
    float h = sz * 0.5f;
    float th = fmaxf(1.2f, sz * 0.13f);
    switch (icon) {
        case HudIcon::Fps:
            for (int i = 0; i < 3; i++) {
                float bx = cx - h + i * sz * 0.38f;
                float bh = sz * (0.4f + i * 0.3f);
                dl->AddRectFilled(ImVec2(bx, cy + h - bh), ImVec2(bx + sz * 0.24f, cy + h), c);
            }
            break;
        case HudIcon::Ping:
            for (int i = 0; i < 4; i++) {
                float bx = cx - h + i * sz * 0.28f;
                float bh = sz * (0.25f + i * 0.25f);
                dl->AddRectFilled(ImVec2(bx, cy + h - bh), ImVec2(bx + sz * 0.16f, cy + h), c);
            }
            break;
        case HudIcon::Clock: {
            dl->AddCircle(ImVec2(cx, cy), h, c, 20, th);
            dl->AddLine(ImVec2(cx, cy - h * 0.55f), ImVec2(cx, cy), c, th);
            dl->AddLine(ImVec2(cx, cy), ImVec2(cx + h * 0.45f, cy + h * 0.2f), c, th);
        } break;
        case HudIcon::User: {
            dl->AddCircleFilled(ImVec2(cx, cy - h * 0.35f), h * 0.42f, c, 20);
            // body: half-circle bottom
            const int seg = 14;
            for (int i = 0; i < seg; i++) {
                float a0 = 3.14159f + i * 3.14159f / seg;
                float a1 = 3.14159f + (i + 1) * 3.14159f / seg;
                ImVec2 p0(cx + cosf(a0) * h * 0.85f, cy + h * 1.05f + sinf(a0) * h * 0.85f);
                ImVec2 p1(cx + cosf(a1) * h * 0.85f, cy + h * 1.05f + sinf(a1) * h * 0.85f);
                dl->AddTriangleFilled(ImVec2(cx, cy + h * 1.05f), p0, p1, c);
            }
        } break;
        case HudIcon::Eye: {
            // ellipse outline (approx)
            const int seg = 24;
            ImVec2 prev(cx - h, cy);
            for (int i = 1; i <= seg; i++) {
                float t = (float)i / seg;
                float ang = 3.14159f * (1.f - t * 2.f);
                float x = cx + cosf(ang) * h;
                float y = cy + sinf(ang) * h * 0.5f;
                dl->AddLine(prev, ImVec2(x, y), c, th);
                prev = ImVec2(x, y);
            }
            dl->AddCircleFilled(ImVec2(cx, cy), h * 0.3f, c, 12);
        } break;
        case HudIcon::Music: {
            dl->AddCircleFilled(ImVec2(cx - h * 0.4f, cy + h * 0.4f), h * 0.32f, c, 12);
            dl->AddCircleFilled(ImVec2(cx + h * 0.5f, cy + h * 0.1f), h * 0.32f, c, 12);
            dl->AddLine(ImVec2(cx - h * 0.05f, cy + h * 0.4f), ImVec2(cx - h * 0.05f, cy - h * 0.6f), c, th);
            dl->AddLine(ImVec2(cx - h * 0.05f, cy - h * 0.6f), ImVec2(cx + h * 0.85f, cy - h * 0.9f), c, th);
            dl->AddLine(ImVec2(cx + h * 0.85f, cy - h * 0.9f), ImVec2(cx + h * 0.85f, cy + h * 0.1f), c, th);
        } break;
        case HudIcon::Bomb: {
            dl->AddCircleFilled(ImVec2(cx - h * 0.1f, cy + h * 0.15f), h * 0.7f, c, 20);
            dl->AddLine(ImVec2(cx + h * 0.35f, cy - h * 0.4f), ImVec2(cx + h, cy - h), c, th);
        } break;
    }
}
// ---------- HUD elements ----------

static void DrawWatermark(ImDrawList* dl, const ImGuiIO& io) {
    time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt);
    char timebuf[16]; snprintf(timebuf, sizeof(timebuf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    static std::string user = GetUser();
    char fps[24], ms[24];
    snprintf(fps, sizeof(fps), "%.0f FPS", g_fps_shown);
    snprintf(ms,  sizeof(ms),  "%.0f MS",  0.f);

    struct Seg { const char* txt; ImU32 c; HudIcon icon; bool has_icon; };
    Seg segs[] = {
        { "SPAXER",     C::accent,   HudIcon::Fps,  false },
        { fps,          C::text,     HudIcon::Fps,   true },
        { ms,           C::text_dim, HudIcon::Ping,  true },
        { timebuf,      C::text,     HudIcon::Clock, true },
        { user.c_str(), C::text,     HudIcon::User,  true },
    };
    int n = 5;
    float pad_l = 12.f;
    float pad_seg = 12.f;
    float icon_sz = 12.f;
    float icon_gap = 5.f;
    float h = 28.f;
    float w = pad_l;
    for (int i = 0; i < n; i++) {
        if (segs[i].has_icon) w += icon_sz + icon_gap;
        w += ImGui::CalcTextSize(segs[i].txt).x;
        if (i < n-1) w += pad_seg;
    }
    w += pad_l;
    if (!g_pos_watermark.init) { g_pos_watermark.x = io.DisplaySize.x - w - 14.f; g_pos_watermark.y = 12.f; g_pos_watermark.init = true; }
    ImVec2 tl(g_pos_watermark.x, g_pos_watermark.y);
    ImVec2 br(tl.x + w, tl.y + h);
    HudDrag("wm", g_pos_watermark, tl, br, io);
    GlassPanel(dl, tl, br, 6.f, 1.f);
    float x = tl.x + pad_l;
    float cy = tl.y + h * 0.5f;
    for (int i = 0; i < n; i++) {
        if (segs[i].has_icon) {
            DrawHudIcon(dl, segs[i].icon, x + icon_sz * 0.5f, cy, icon_sz, C::accent);
            x += icon_sz + icon_gap;
        }
        ImVec2 ts = ImGui::CalcTextSize(segs[i].txt);
        dl->AddText(ImVec2(x, cy - ts.y * 0.5f), segs[i].c, segs[i].txt);
        x += ts.x + pad_seg;
    }
}

static void DrawKeystrokes(ImDrawList* dl, const ImGuiIO& io) {
    float k = 28.f, g = 4.f;
    float total_w = k * 3 + g * 2;
    if (!g_pos_keystrokes.init) { g_pos_keystrokes.x = io.DisplaySize.x * 0.5f - total_w * 0.5f; g_pos_keystrokes.y = io.DisplaySize.y - 130.f; g_pos_keystrokes.init = true; }
    float cx = g_pos_keystrokes.x;
    float cy = g_pos_keystrokes.y;
    HudDrag("keys", g_pos_keystrokes, ImVec2(cx - 8, cy - 8), ImVec2(cx + total_w + k*1.6f + g + 8, cy + 3*(k+g) + 8), io);

    auto Key = [&](const char* lbl, int id, float x, float y, float w) {
        bool on = SpaxerKey(id) != 0;
        ImVec2 tl(x, y), br(x + w, y + k);
        dl->AddRectFilled(tl, br, on ? C::accent : IM_COL32(0x11, 0x13, 0x1a, 220), 4.f);
        dl->AddRect(tl, br, on ? C::accent : C::root_bd, 4.f, 0, 1.f);
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(tl.x + (w - ts.x) * 0.5f, tl.y + (k - ts.y) * 0.5f), on ? IM_COL32(255,255,255,255) : C::text_row, lbl);
    };
    Key("W", 0, cx + k + g,     cy,               k);
    Key("A", 1, cx,             cy + k + g,       k);
    Key("S", 2, cx + k + g,     cy + k + g,       k);
    Key("D", 3, cx + 2*(k+g),   cy + k + g,       k);
    Key("SPACE", 4, cx,         cy + 2*(k+g),     total_w);
    Key("SHIFT", 5, cx + total_w + g, cy + k + g, k * 1.6f);
}

// Flags
extern bool g_watermark, g_keystrokes, g_hotkeys_hud;
extern bool g_aimbot, g_triggerbot, g_esp_box, g_esp_skeleton, g_esp_health, g_esp_name, g_chams, g_bhop;
bool g_watermark = true;
bool g_keystrokes = true;
bool g_hotkeys_hud = true;
bool g_aimbot = false;
bool g_triggerbot = false;
bool g_esp_box = true;
bool g_esp_skeleton = true;
bool g_esp_health = true;
bool g_esp_name = true;
bool g_chams = false;
bool g_bhop = false;
static bool g_esp_enemies_only = true;
static bool g_esp_weapon = true;
static bool g_esp_ammo = true;
static bool g_esp_flags = true;
static bool g_glow = false;
static bool g_hitmarker = true;
static bool g_radar = true;
static bool g_bomb_timer = true;
static bool g_spectators = true;
static bool g_media = true;
static bool g_notif = true;
static bool g_thirdperson = false;
static bool g_noflash = true;
static bool g_smoke_recolor = true;
static bool g_rcs = true;
static bool g_hum = true;
static bool g_autojump = true;
static bool g_autostrafe = true;
static bool g_saturation = true;
static bool g_night = false;
static bool g_crosshair_custom = false;
static bool g_crosshair_dot = false;
static bool g_crosshair_outline = true;
static bool g_arrows = true;
static bool g_sound_esp = true;
static bool g_dropped_items = true;
static bool g_grenade_pred = false;
static float g_crosshair_size = 6.f;
static float g_crosshair_gap = 4.f;
static float g_crosshair_thickness = 1.4f;
static float g_fov = 64.6f;
static float g_speed = 39.f;
static float g_sens = 123.f;
static float g_switch_delay = 150.f;
static float g_rcs_strength = 80.f;
static float g_hit_ms = 60.f;
static float g_hitc = 60.f;
static float g_mind = 20.f;
static float g_strength = 80.f;
static float g_smax = 37.f;
static float g_smin = 20.f;
static float g_shake = 17.f;
static float g_release = 35.f;
static float g_brightness = 100.f;
static float g_night_strength = 60.f;
static float g_sat_pct = 244.f;
static float g_wdensity = 60.f;
static float g_smoke_str = 255.f;
static float g_hitvol = 70.f;
static int g_tab = 0;

static void DrawHotkeys(ImDrawList* dl, const ImGuiIO& io) {
    struct H { const char* name; bool on; const char* key; };
    H hs[] = {
        {"Aimbot",     g_aimbot,     "[R-Click]"},
        {"Triggerbot", g_triggerbot, "[Shift]"},
        {"Bhop",       g_bhop,       "[Space]"},
    };
    int n = 0; for (auto& h : hs) if (h.on) n++;
    if (n == 0) return;
    float w = 220, rowh = 22, headh = 28;
    if (!g_pos_hotkeys.init) { g_pos_hotkeys.x = 20; g_pos_hotkeys.y = 100; g_pos_hotkeys.init = true; }
    float x = g_pos_hotkeys.x, y = g_pos_hotkeys.y;
    float h = headh + n * rowh + 12;
    HudDrag("hot", g_pos_hotkeys, ImVec2(x, y), ImVec2(x + w, y + h), io);
    GlassPanel(dl, ImVec2(x, y), ImVec2(x + w, y + h), 8.f, 1.f);
    dl->AddLine(ImVec2(x, y + headh - 1), ImVec2(x + w, y + headh - 1), C::soft_bd, 1.f);
    dl->AddText(ImVec2(x + 16, y + 8), C::sec_head, "HOTKEYS");
    float ty = y + headh + 6;
    for (auto& h : hs) if (h.on) {
        dl->AddText(ImVec2(x + 16, ty), C::text_row, h.name);
        ImVec2 ks = ImGui::CalcTextSize(h.key);
        dl->AddText(ImVec2(x + w - ks.x - 16, ty), C::text_dim, h.key);
        ty += rowh;
    }
}


// ---------- Bomb timer (top center) ----------

static bool  g_fake_bomb_active = true;
static float g_fake_bomb_time   = 40.f;

static void DrawBomb(ImDrawList* dl, const ImGuiIO& io, float dt) {
    if (!g_bomb_timer) return;
    // fake countdown for demo purposes
    g_fake_bomb_time -= dt;
    if (g_fake_bomb_time < 0.f) g_fake_bomb_time = 40.f;
    float t = g_fake_bomb_time;
    float total = 40.f;
    float frac = t / total;

    float W = 220, H = 26;
    if (!g_pos_bomb.init) { g_pos_bomb.x = io.DisplaySize.x * 0.5f - W * 0.5f; g_pos_bomb.y = 76.f; g_pos_bomb.init = true; }
    ImVec2 tl(g_pos_bomb.x, g_pos_bomb.y);
    ImVec2 br(tl.x + W, tl.y + H);
    HudDrag("bomb", g_pos_bomb, tl, br, io);
    GlassPanel(dl, tl, br, 6.f, 1.f);
    // icon (small bomb circle)
    ImU32 col_urgent = (t < 10.f) ? IM_COL32(255, 90, 90, 255) : C::accent;
    dl->AddCircleFilled(ImVec2(tl.x + 16, tl.y + H*0.5f), 5.f, col_urgent);
    // text
    char buf[32]; snprintf(buf, sizeof(buf), "Site A   %.1fs", t);
    dl->AddText(ImVec2(tl.x + 30, tl.y + 6), C::text, buf);
    // progress
    dl->AddRectFilled(ImVec2(tl.x + 8, br.y - 3), ImVec2(tl.x + 8 + (W - 16) * frac, br.y - 1),
                      col_urgent, 1.f);
}

// ---------- Velocity graph (bottom center) ----------

static float g_vel_hist[128] = {};
static int g_vel_idx = 0;
static float g_vel_time = 0.f;

static void DrawVelocity(ImDrawList* dl, const ImGuiIO& io, float dt) {
    if (!g_media) return; // reused flag "velocity graph" toggle
    g_vel_time += dt;
    // fake sinusoidal velocity 0..300
    float v = 150.f + 130.f * sinf(g_vel_time * 2.5f) + 20.f * sinf(g_vel_time * 7.f);
    g_vel_hist[g_vel_idx] = v;
    g_vel_idx = (g_vel_idx + 1) % 128;

    float W = 260, H = 62;
    if (!g_pos_velocity.init) { g_pos_velocity.x = io.DisplaySize.x * 0.5f - W * 0.5f; g_pos_velocity.y = io.DisplaySize.y - 240.f; g_pos_velocity.init = true; }
    ImVec2 tl(g_pos_velocity.x, g_pos_velocity.y);
    ImVec2 br(tl.x + W, tl.y + H);
    HudDrag("vel", g_pos_velocity, tl, br, io);
    GlassPanel(dl, tl, br, 6.f, 1.f);
    char lbl[32]; snprintf(lbl, sizeof(lbl), "%.0f u/s", v);
    dl->AddText(ImVec2(tl.x + 10, tl.y + 4), C::text_dim, "Velocity");
    ImVec2 ts = ImGui::CalcTextSize(lbl);
    dl->AddText(ImVec2(br.x - ts.x - 10, tl.y + 4), C::accent, lbl);
    // graph
    float gx1 = tl.x + 6, gx2 = br.x - 6;
    float gy1 = tl.y + 22, gy2 = br.y - 6;
    ImVec2 prev(0,0); bool has_prev = false;
    for (int i = 0; i < 128; i++) {
        int idx = (g_vel_idx + i) % 128;
        float x = gx1 + (gx2 - gx1) * ((float)i / 127.f);
        float vv = g_vel_hist[idx] / 350.f;
        if (vv > 1) vv = 1; if (vv < 0) vv = 0;
        float y = gy2 - (gy2 - gy1) * vv;
        if (has_prev) {
            dl->AddLine(prev, ImVec2(x, y), C::accent, 1.4f);
            // area under
            dl->AddTriangleFilled(prev, ImVec2(x, y), ImVec2(x, gy2), IM_COL32(0x4c, 0x8d, 0xff, 40));
            dl->AddTriangleFilled(prev, ImVec2(x, gy2), ImVec2(prev.x, gy2), IM_COL32(0x4c, 0x8d, 0xff, 40));
        }
        prev = ImVec2(x, y); has_prev = true;
    }
}

// ---------- Spectators list (right) ----------

static void DrawSpectators(ImDrawList* dl, const ImGuiIO& io) {
    if (!g_spectators) return;
    const char* players[] = { "Example player" };
    int n = 1;
    float W = 180, RH = 24, HD = 28;
    float H = HD + n * RH + 6;
    if (!g_pos_spectators.init) { g_pos_spectators.x = io.DisplaySize.x - W - 14; g_pos_spectators.y = 60; g_pos_spectators.init = true; }
    ImVec2 tl(g_pos_spectators.x, g_pos_spectators.y);
    ImVec2 br(tl.x + W, tl.y + H);
    HudDrag("spec", g_pos_spectators, tl, br, io);
    GlassPanel(dl, tl, br, 6.f, 1.f);
    SectionHeader(dl, tl, br, "Spectators");
    for (int i = 0; i < n; i++) {
        float y = tl.y + HD + i * RH;
        // eye icon
        dl->AddCircle(ImVec2(tl.x + 18, y + 12), 4.5f, C::text_dim, 0, 1.2f);
        dl->AddCircleFilled(ImVec2(tl.x + 18, y + 12), 1.6f, C::text_dim);
        dl->AddText(ImVec2(tl.x + 32, y + 6), C::text_row, players[i]);
    }
}

// ---------- Media player (left bottom) ----------

static void DrawMedia(ImDrawList* dl, const ImGuiIO& io) {
    if (!g_media) return; // reused
    float W = 240, H = 92;
    if (!g_pos_media.init) { g_pos_media.x = 20; g_pos_media.y = io.DisplaySize.y - H - 30; g_pos_media.init = true; }
    ImVec2 tl(g_pos_media.x, g_pos_media.y);
    ImVec2 br(tl.x + W, tl.y + H);
    HudDrag("media", g_pos_media, tl, br, io);
    GlassPanel(dl, tl, br, 6.f, 1.f);
    SectionHeader(dl, tl, br, "Media");
    dl->AddText(ImVec2(tl.x + 10, tl.y + 30), C::text, "No track");
    dl->AddText(ImVec2(tl.x + 10, tl.y + 48), C::text_dim, "spaxer.internal");
    // controls
    float cy = tl.y + H - 18;
    float cx = tl.x + W * 0.5f;
    // prev
    dl->AddTriangleFilled(ImVec2(cx - 30, cy), ImVec2(cx - 22, cy - 5), ImVec2(cx - 22, cy + 5), C::text_dim);
    dl->AddLine(ImVec2(cx - 32, cy - 5), ImVec2(cx - 32, cy + 5), C::text_dim, 1.4f);
    // play
    dl->AddTriangleFilled(ImVec2(cx - 5, cy - 6), ImVec2(cx - 5, cy + 6), ImVec2(cx + 6, cy), C::accent);
    // next
    dl->AddLine(ImVec2(cx + 32, cy - 5), ImVec2(cx + 32, cy + 5), C::text_dim, 1.4f);
    dl->AddTriangleFilled(ImVec2(cx + 30, cy), ImVec2(cx + 22, cy - 5), ImVec2(cx + 22, cy + 5), C::text_dim);
}

// ---------- Notifications (right side stack) ----------

struct Notif { std::string text; float t; ImU32 color; };
static std::vector<Notif> g_notifs;
static float g_notif_test_timer = 0.f;

extern "C" void SpaxerNotify(const char* text, uint32_t color = 0) {
    Notif n{text, 3.5f, color ? color : (ImU32)C::accent};
    g_notifs.push_back(std::move(n));
    if (g_notifs.size() > 6) g_notifs.erase(g_notifs.begin());
}

static void DrawNotifs(ImDrawList* dl, const ImGuiIO& io, float dt) {
    if (!g_notif) return;
    g_notif_test_timer += dt;
    // demo: cycle through sample notices
    if (g_notif_test_timer > 4.5f) {
        g_notif_test_timer = 0.f;
        struct D { const char* t; ImU32 c; };
        D demos[] = {
            { "Config loaded",     IM_COL32( 88, 132, 255, 255) },
            { "Hit for 45 damage", IM_COL32( 56, 108, 255, 255) },
            { "Enemy killed",      IM_COL32( 30, 170, 100, 255) },
            { "Bomb planted",      IM_COL32(255, 150,  55, 255) },
            { "Aimbot enabled",    IM_COL32(120, 200, 255, 255) },
        };
        static int idx = 0;
        idx = (idx + 1) % (int)(sizeof(demos)/sizeof(demos[0]));
        SpaxerNotify(demos[idx].t, demos[idx].c);
    }
    for (auto& n : g_notifs) n.t -= dt;
    g_notifs.erase(std::remove_if(g_notifs.begin(), g_notifs.end(), [](const Notif& n){ return n.t <= 0.f; }), g_notifs.end());

    // Center-top stack (like external)
    float W = 320, H = 36;
    float cx = io.DisplaySize.x * 0.5f;
    float y = io.DisplaySize.y * 0.22f;
    for (auto it = g_notifs.rbegin(); it != g_notifs.rend(); ++it) {
        float a = it->t > 0.4f ? 1.f : it->t / 0.4f;
        if (it->t > 3.1f) a = (3.5f - it->t) / 0.4f; // fade in
        if (a < 0) a = 0; if (a > 1) a = 1;
        ImVec2 tl(cx - W * 0.5f, y);
        ImVec2 br(tl.x + W, tl.y + H);
        GlassPanel(dl, tl, br, 9.f, a);
        // color tint left→right
        ImU32 ct = it->color;
        int cr = (ct >> IM_COL32_R_SHIFT) & 0xFF;
        int cg = (ct >> IM_COL32_G_SHIFT) & 0xFF;
        int cb = (ct >> IM_COL32_B_SHIFT) & 0xFF;
        ImU32 t_left  = IM_COL32(cr, cg, cb, (int)(96 * a));
        ImU32 t_right = IM_COL32(cr, cg, cb, (int)(10 * a));
        dl->AddRectFilledMultiColor(tl, br, t_left, t_right, t_right, t_left);
        // icon circle
        float icy = tl.y + H * 0.5f;
        float icx = tl.x + H * 0.5f + 4.f;
        dl->AddCircleFilled(ImVec2(icx, icy), 11.f, IM_COL32(cr, cg, cb, (int)(240 * a)));
        dl->AddCircleFilled(ImVec2(icx, icy), 4.f, IM_COL32(255, 255, 255, (int)(255 * a)));
        // text
        dl->AddText(ImVec2(tl.x + H + 6, tl.y + 11), IM_COL32(240, 245, 255, (int)(255 * a)), it->text.c_str());
        // life bar
        float life = it->t / 3.5f; if (life < 0) life = 0; if (life > 1) life = 1;
        dl->AddRectFilled(ImVec2(tl.x + 10, br.y - 3), ImVec2(tl.x + 10 + (W - 20) * life, br.y - 1), IM_COL32(cr, cg, cb, (int)(200 * a)), 1.f);
        y += H + 8.f;
    }
}

// ---------- Hitmarker (fake demo) ----------

static float g_hit_timer = 0.f;
static struct HitMark { float x, y; int dmg; float t; } g_hits[16] = {};
static int g_hit_idx = 0;

static void DrawHitmarker(ImDrawList* dl, const ImGuiIO& io, float dt) {
    if (!g_hitmarker) return;
    g_hit_timer += dt;
    if (g_hit_timer > 2.0f) {
        g_hit_timer = 0.f;
        float cx = io.DisplaySize.x * 0.5f, cy = io.DisplaySize.y * 0.5f;
        g_hits[g_hit_idx].x = cx + (rand() % 200 - 100);
        g_hits[g_hit_idx].y = cy + (rand() % 200 - 100);
        g_hits[g_hit_idx].dmg = 20 + rand() % 80;
        g_hits[g_hit_idx].t = 0.8f;
        g_hit_idx = (g_hit_idx + 1) % 16;
    }
    for (auto& h : g_hits) {
        if (h.t <= 0) continue;
        h.t -= dt;
        float a = h.t / 0.8f; if (a < 0) a = 0;
        float age = 1.f - a;
        float len = 6.f;
        // outer black
        dl->AddLine(ImVec2(h.x - len, h.y - len), ImVec2(h.x + len, h.y + len), IM_COL32(0, 0, 0, (int)(200 * a)), 3.5f);
        dl->AddLine(ImVec2(h.x + len, h.y - len), ImVec2(h.x - len, h.y + len), IM_COL32(0, 0, 0, (int)(200 * a)), 3.5f);
        // white
        dl->AddLine(ImVec2(h.x - len, h.y - len), ImVec2(h.x + len, h.y + len), IM_COL32(255, 255, 255, (int)(255 * a)), 1.8f);
        dl->AddLine(ImVec2(h.x + len, h.y - len), ImVec2(h.x - len, h.y + len), IM_COL32(255, 255, 255, (int)(255 * a)), 1.8f);
        // damage
        char b[16]; snprintf(b, sizeof(b), "-%d", h.dmg);
        dl->AddText(ImVec2(h.x + 9, h.y - 20 - age * 30), IM_COL32(140, 190, 255, (int)(255 * a)), b);
    }
}

// ---------- Radar (top-left circle) ----------

static void DrawRadar(ImDrawList* dl, const ImGuiIO& io) {
    if (!g_radar) return;
    float R = 90.f;
    if (!g_pos_radar.init) { g_pos_radar.x = 24; g_pos_radar.y = 24; g_pos_radar.init = true; }
    ImVec2 tl(g_pos_radar.x, g_pos_radar.y);
    ImVec2 br(tl.x + 2 * R, tl.y + 2 * R);
    HudDrag("radar", g_pos_radar, tl, br, io);
    ImVec2 c(tl.x + R, tl.y + R);
    // dark circular bg
    dl->AddCircleFilled(c, R, IM_COL32(0, 0, 0, 210), 64);
    // 3 concentric rings
    for (int i = 1; i <= 3; i++)
        dl->AddCircle(c, R * i / 3.f, IM_COL32(255, 255, 255, 22), 64, 1.f);
    // cross
    dl->AddLine(ImVec2(c.x - R + 4, c.y), ImVec2(c.x + R - 4, c.y), IM_COL32(255, 255, 255, 20), 1.f);
    dl->AddLine(ImVec2(c.x, c.y - R + 4), ImVec2(c.x, c.y + R - 4), IM_COL32(255, 255, 255, 20), 1.f);
    // outer accent ring
    dl->AddCircle(c, R, IM_COL32(80, 130, 200, 120), 64, 1.5f);
    // player triangle (looking up)
    dl->AddTriangleFilled(ImVec2(c.x, c.y - 6), ImVec2(c.x + 5, c.y + 5), ImVec2(c.x - 5, c.y + 5), IM_COL32(240, 245, 255, 240));
}



// ---------- Crosshair ----------

static void DrawCrosshair(ImDrawList* dl, const ImGuiIO& io) {
    if (!g_crosshair_custom) return;
    float cx = io.DisplaySize.x * 0.5f, cy = io.DisplaySize.y * 0.5f;
    float gap = g_crosshair_gap, len = g_crosshair_size, th = g_crosshair_thickness;
    ImU32 col = C::accent;
    if (g_crosshair_outline) {
        ImU32 out = IM_COL32(0, 0, 0, 190);
        float ot = th + 1.5f;
        dl->AddLine(ImVec2(cx - gap - len, cy), ImVec2(cx - gap, cy), out, ot);
        dl->AddLine(ImVec2(cx + gap, cy),       ImVec2(cx + gap + len, cy), out, ot);
        dl->AddLine(ImVec2(cx, cy - gap - len), ImVec2(cx, cy - gap), out, ot);
        dl->AddLine(ImVec2(cx, cy + gap),       ImVec2(cx, cy + gap + len), out, ot);
    }
    dl->AddLine(ImVec2(cx - gap - len, cy), ImVec2(cx - gap, cy), col, th);
    dl->AddLine(ImVec2(cx + gap, cy),       ImVec2(cx + gap + len, cy), col, th);
    dl->AddLine(ImVec2(cx, cy - gap - len), ImVec2(cx, cy - gap), col, th);
    dl->AddLine(ImVec2(cx, cy + gap),       ImVec2(cx, cy + gap + len), col, th);
    if (g_crosshair_dot) dl->AddCircleFilled(ImVec2(cx, cy), th * 0.6f, col);
}

// ---------- Off-screen arrows ----------

static void DrawArrows(ImDrawList* dl, const ImGuiIO& io) {
    if (!g_arrows) return;
    // demo: 3 rotating markers around screen center
    static float t = 0; t += 0.008f;
    float cx = io.DisplaySize.x * 0.5f, cy = io.DisplaySize.y * 0.5f;
    float rad = fminf(cx, cy) - 90.f;
    for (int i = 0; i < 3; i++) {
        float ang = t + i * 2.09f;
        float px = cx + cosf(ang) * rad;
        float py = cy + sinf(ang) * rad;
        // triangle pointing outward
        float nx = cosf(ang), ny = sinf(ang);
        float tx = -ny, ty = nx;
        ImVec2 a(px + nx * 12, py + ny * 12);
        ImVec2 b(px + tx * 8,  py + ty * 8);
        ImVec2 c(px - tx * 8,  py - ty * 8);
        dl->AddTriangleFilled(a, b, c, IM_COL32(255, 90, 90, 220));
        dl->AddTriangle(a, b, c, IM_COL32(0, 0, 0, 200), 1.4f);
        // distance
        char d[16]; snprintf(d, sizeof(d), "%dm", 12 + i * 7);
        dl->AddText(ImVec2(px - 10, py + 14), IM_COL32(255, 220, 220, 220), d);
    }
}

// ---------- Sound ESP ----------

static struct Snd { float x, y, r, life; } g_snd[8] = {};
static int g_snd_idx = 0;
static float g_snd_timer = 0.f;

static void DrawSoundEsp(ImDrawList* dl, const ImGuiIO& io, float dt) {
    if (!g_sound_esp) return;
    g_snd_timer += dt;
    if (g_snd_timer > 1.3f) {
        g_snd_timer = 0.f;
        g_snd[g_snd_idx].x = 100.f + (rand() % (int)(io.DisplaySize.x - 200));
        g_snd[g_snd_idx].y = 100.f + (rand() % (int)(io.DisplaySize.y - 200));
        g_snd[g_snd_idx].r = 8.f;
        g_snd[g_snd_idx].life = 1.5f;
        g_snd_idx = (g_snd_idx + 1) % 8;
    }
    for (auto& s : g_snd) {
        if (s.life <= 0) continue;
        s.life -= dt;
        s.r += dt * 50.f;
        float a = s.life / 1.5f; if (a < 0) a = 0;
        dl->AddCircle(ImVec2(s.x, s.y), s.r, IM_COL32(140, 200, 255, (int)(180 * a)), 32, 1.6f);
    }
}

// ---------- Dropped items ----------

static void DrawDroppedItems(ImDrawList* dl, const ImGuiIO& io) {
    if (!g_dropped_items) return;
    // demo: 4 static "weapons" at pseudo-screen positions
    const char* items[] = { "AK-47", "AWP", "USP-S", "Grenade" };
    ImVec2 positions[] = {
        {io.DisplaySize.x * 0.30f, io.DisplaySize.y * 0.60f},
        {io.DisplaySize.x * 0.55f, io.DisplaySize.y * 0.72f},
        {io.DisplaySize.x * 0.70f, io.DisplaySize.y * 0.55f},
        {io.DisplaySize.x * 0.45f, io.DisplaySize.y * 0.80f},
    };
    for (int i = 0; i < 4; i++) {
        ImVec2 p = positions[i];
        dl->AddCircleFilled(p, 3.f, C::accent);
        ImVec2 ts = ImGui::CalcTextSize(items[i]);
        ImVec2 tl(p.x - ts.x * 0.5f - 6, p.y + 6);
        ImVec2 br(p.x + ts.x * 0.5f + 6, p.y + 24);
        dl->AddRectFilled(tl, br, IM_COL32(0, 0, 0, 180), 3.f);
        dl->AddText(ImVec2(p.x - ts.x * 0.5f, p.y + 8), C::text, items[i]);
    }
}

// ---------- Grenade prediction ----------

static void DrawGrenadePred(ImDrawList* dl, const ImGuiIO& io) {
    if (!g_grenade_pred) return;
    // demo arc from screen center
    float cx = io.DisplaySize.x * 0.5f, cy = io.DisplaySize.y * 0.7f;
    float apex_x = cx + 200, apex_y = cy - 250;
    float land_x = cx + 500, land_y = cy - 40;
    // 20 segments quadratic
    ImVec2 prev(cx, cy);
    for (int i = 1; i <= 30; i++) {
        float t = i / 30.f;
        float x = (1-t)*(1-t)*cx + 2*(1-t)*t*apex_x + t*t*land_x;
        float y = (1-t)*(1-t)*cy + 2*(1-t)*t*apex_y + t*t*land_y;
        ImU32 col = IM_COL32(120, 200, 255, 200 - i * 3);
        dl->AddLine(prev, ImVec2(x, y), col, 2.f);
        prev = ImVec2(x, y);
    }
    dl->AddCircleFilled(prev, 5.f, IM_COL32(255, 90, 90, 220));
    dl->AddCircle(prev, 10.f, IM_COL32(255, 90, 90, 130), 32, 2.f);
}

// ---------- Real ESP (in-process memory reads) ----------

static bool WorldToScreen(const float* m, float x, float y, float z, float w, float h, float& sx, float& sy) {
    float xw = m[0]*x + m[1]*y + m[2]*z + m[3];
    float yw = m[4]*x + m[5]*y + m[6]*z + m[7];
    float zw = m[12]*x + m[13]*y + m[14]*z + m[15];
    if (zw < 0.01f) return false;
    sx = w * 0.5f + (xw / zw) * w * 0.5f;
    sy = h * 0.5f - (yw / zw) * h * 0.5f;
    return true;
}

static void DrawEspReal(ImDrawList* dl, const ImGuiIO& io) {
    if (!g_esp_ready.load()) return;
    EspSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(g_esp_mtx);
        snap = g_esp_snap;
    }
    if (!snap.valid) return;
    int local_team = 0;
    for (auto& p : snap.players) if (p.local) { local_team = p.team; break; }
    for (auto& p : snap.players) {
        if (p.local) continue;
        if (p.dormant) continue;
        bool enemy = (local_team == 0) || (p.team != local_team);
        float fsx, fsy, hsx, hsy;
        if (!WorldToScreen(snap.view_matrix, p.ox, p.oy, p.oz, io.DisplaySize.x, io.DisplaySize.y, fsx, fsy)) continue;
        if (!WorldToScreen(snap.view_matrix, p.hx, p.hy, p.hz, io.DisplaySize.x, io.DisplaySize.y, hsx, hsy)) continue;
        float box_h = fsy - hsy;
        float box_w = box_h * 0.55f;
        ImVec2 tl(fsx - box_w * 0.5f, hsy);
        ImVec2 br(fsx + box_w * 0.5f, fsy);
        ImU32 col = enemy ? IM_COL32(255, 90, 90, 255) : IM_COL32(90, 220, 120, 255);
        if (g_esp_box) {
            dl->AddRect(tl, br, IM_COL32(0, 0, 0, 220), 0, 0, 3.f);
            dl->AddRect(tl, br, col, 0, 0, 1.4f);
        }
        if (g_esp_health) {
            float hfrac = p.health / 100.f;
            float bx = tl.x - 6, by = br.y - box_h * hfrac;
            ImU32 hc = IM_COL32(50 + (int)(hfrac*30), 200, 90, 255);
            dl->AddRectFilled(ImVec2(bx - 2, tl.y - 1), ImVec2(bx + 4, br.y + 1), IM_COL32(0, 0, 0, 220));
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + 2, br.y), hc);
        }
        if (g_esp_name && !p.name.empty()) {
            ImVec2 ts = ImGui::CalcTextSize(p.name.c_str());
            dl->AddText(ImVec2(fsx - ts.x * 0.5f + 1, tl.y - 16 + 1), IM_COL32(0, 0, 0, 220), p.name.c_str());
            dl->AddText(ImVec2(fsx - ts.x * 0.5f, tl.y - 16), IM_COL32(230, 235, 250, 255), p.name.c_str());
        }
    }
}

// ---------- Menu widgets ----------

static bool DoToggle(const char* id, bool* v) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float sw = 34.f, sh = 18.f;
    ImVec2 tl(pos.x, pos.y);
    ImVec2 br(pos.x + sw, pos.y + sh);
    ImRect rect(tl, br);
    ImGuiID iid = w->GetID(id);
    ImGui::ItemAdd(rect, iid);
    ImGui::ItemSize(ImVec2(sw, sh));
    bool hov, held;
    bool pressed = ImGui::ButtonBehavior(rect, iid, &hov, &held);
    if (pressed) *v = !*v;
    ImDrawList* dl = w->DrawList;
    float r = sh * 0.5f;
    if (*v) {
        // Filled blue pill with subtle inner brighter edge
        dl->AddRectFilled(tl, br, C::accent, r);
        // tiny highlight on top
        dl->AddLine(ImVec2(tl.x + 4, tl.y + 1), ImVec2(br.x - 4, tl.y + 1), IM_COL32(255, 255, 255, 60), 1.f);
    } else {
        dl->AddRectFilled(tl, br, C::toggle_off_bg, r);
        dl->AddRect(tl, br, C::toggle_off_bd, r, 0, 1.f);
    }
    float kr = r - 2.f;
    float kcy = tl.y + r;
    float kcx = *v ? (br.x - kr - 2.f) : (tl.x + kr + 2.f);
    dl->AddCircleFilled(ImVec2(kcx, kcy), kr, *v ? C::knob_white : C::toggle_off_kb, 16);
    return pressed;
}

static void ToggleRow(const char* label, bool* v, const char* chip=nullptr) {
    float row_h = 30.f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(p.x + 4, p.y + (row_h - ImGui::GetFontSize()) * 0.5f), C::text_row, label);
    float toggle_x = p.x + avail - 34.f - 6.f;
    if (chip) {
        ImVec2 cts = ImGui::CalcTextSize(chip);
        float chip_w = cts.x + 16.f;
        ImVec2 chip_tl(toggle_x - chip_w - 8, p.y + (row_h - 20) * 0.5f);
        ImVec2 chip_br(chip_tl.x + chip_w, chip_tl.y + 20);
        dl->AddRectFilled(chip_tl, chip_br, IM_COL32(0x4c, 0x8d, 0xff, 30), 4.f);
        dl->AddRect(chip_tl, chip_br, IM_COL32(0x4c, 0x8d, 0xff, 70), 4.f, 0, 1.f);
        dl->AddText(ImVec2(chip_tl.x + 8, chip_tl.y + 3), IM_COL32(0x7f, 0xa7, 0xff, 255), chip);
    }
    ImGui::SetCursorScreenPos(ImVec2(toggle_x, p.y + (row_h - 18) * 0.5f));
    ImGui::PushID(label);
    DoToggle("##t", v);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h));
    ImGui::Dummy(ImVec2(0, 0));
}

static void SliderRow(const char* label, float* v, float lo, float hi, const char* suf="") {
    float row_h = 32.f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // label
    dl->AddText(ImVec2(p.x + 4, p.y + (row_h - ImGui::GetFontSize()) * 0.5f), C::text_row, label);
    // value on far right in BLUE bold-look
    char valbuf[24];
    bool floaty = (hi <= 10.f);
    snprintf(valbuf, sizeof(valbuf), floaty ? "%.2f%s" : "%.0f%s", *v, suf);
    ImVec2 vs = ImGui::CalcTextSize(valbuf);
    dl->AddText(ImVec2(p.x + avail - vs.x - 2, p.y + (row_h - vs.y) * 0.5f), C::accent, valbuf);
    // track
    float trk_y = p.y + row_h * 0.5f;
    float trk_x1 = p.x + avail * 0.42f;
    float trk_x2 = p.x + avail - vs.x - 14.f;
    dl->AddRectFilled(ImVec2(trk_x1, trk_y - 2), ImVec2(trk_x2, trk_y + 2), C::slider_bg, 2.f);
    float t = (*v - lo) / (hi - lo);
    if (t < 0) t = 0; if (t > 1) t = 1;
    float fx = trk_x1 + (trk_x2 - trk_x1) * t;
    // gradient fill
    dl->AddRectFilledMultiColor(ImVec2(trk_x1, trk_y - 2), ImVec2(fx, trk_y + 2), C::accent2, C::accent, C::accent, C::accent2);
    // large white knob with blue border
    dl->AddCircleFilled(ImVec2(fx, trk_y), 8.f, C::knob_white, 24);
    dl->AddCircle(ImVec2(fx, trk_y), 8.f, C::accent, 24, 2.f);
    // hit test wider
    ImGui::PushID(label);
    ImGui::SetCursorScreenPos(ImVec2(trk_x1 - 4, trk_y - 12));
    ImGui::InvisibleButton("##hit", ImVec2(trk_x2 - trk_x1 + 8, 24));
    if (ImGui::IsItemActive()) {
        float mx = ImGui::GetIO().MousePos.x;
        float nt = (mx - trk_x1) / (trk_x2 - trk_x1);
        if (nt < 0) nt = 0; if (nt > 1) nt = 1;
        *v = lo + (hi - lo) * nt;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h));
}

static void ComboRow(const char* label, int* v, const char* const* items, int n) {
    float row_h = 30.f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(p.x + 4, p.y + (row_h - ImGui::GetFontSize()) * 0.5f), C::text_row, label);
    float cbw = 120.f;
    ImVec2 tl(p.x + avail - cbw - 6, p.y + 4);
    ImVec2 br(tl.x + cbw, tl.y + 22);
    dl->AddRectFilled(tl, br, C::field_bg, 5.f);
    dl->AddRect(tl, br, C::field_bd, 5.f, 0, 1.f);
    dl->AddText(ImVec2(tl.x + 10, tl.y + 4), C::text, items[*v]);
    dl->AddTriangleFilled(ImVec2(br.x - 12, tl.y + 8), ImVec2(br.x - 6, tl.y + 8), ImVec2(br.x - 9, tl.y + 13), C::text_dim);
    ImGui::PushID(label);
    ImGui::SetCursorScreenPos(tl);
    if (ImGui::InvisibleButton("##c", ImVec2(cbw, 22))) *v = (*v + 1) % n;
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h));
    ImGui::Dummy(ImVec2(0, 0));
}

static void Section(const char* head, std::function<void()> body) {
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(12/255.f, 16/255.f, 28/255.f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(22/255.f, 30/255.f, 50/255.f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
    std::string id = std::string("##sec_") + head;
    ImGui::BeginChild(id.c_str(), ImVec2(avail, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float ww = ImGui::GetWindowWidth();
    
    d->AddText(ImVec2(wp.x + 14, wp.y + 11), C::sec_head, head);
    d->AddLine(ImVec2(wp.x + 14, wp.y + 31), ImVec2(wp.x + ww - 14, wp.y + 31), C::soft_bd, 1.f);
    ImGui::Dummy(ImVec2(0, 26));
    body();
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::Spacing();
}

static void SidebarLabel(ImDrawList* dl, float x, float y, const char* text) {
    dl->AddText(ImVec2(x, y), C::sec_head, text);
}

enum SbIcon { ICO_LEGIT, ICO_MOVE, ICO_ESP, ICO_HUD, ICO_WORLD, ICO_MISC, ICO_SCRIPTS };

static void DrawIcon(ImDrawList* dl, float cx, float cy, float sz, SbIcon k, ImU32 c) {
    auto P = [&](float a, float b){ return ImVec2(cx - sz*0.5f + a * sz / 24.f, cy - sz*0.5f + b * sz / 24.f); };
    float th = 1.4f;
    switch (k) {
        case ICO_LEGIT: {
            dl->AddCircle(P(12,12), sz * (8.f/24.f), c, 0, th);
            dl->AddLine(P(12,1.5f), P(12,6), c, th);
            dl->AddLine(P(12,18), P(12,22.5f), c, th);
            dl->AddLine(P(1.5f,12), P(6,12), c, th);
            dl->AddLine(P(18,12), P(22.5f,12), c, th);
            dl->AddCircleFilled(P(12,12), sz * (1.2f/24.f), c);
        } break;
        case ICO_MOVE: {
            dl->AddPolyline((const ImVec2[]){P(5,9),P(2,12),P(5,15)}, 3, c, 0, th);
            dl->AddPolyline((const ImVec2[]){P(9,5),P(12,2),P(15,5)}, 3, c, 0, th);
            dl->AddPolyline((const ImVec2[]){P(15,19),P(12,22),P(9,19)}, 3, c, 0, th);
            dl->AddPolyline((const ImVec2[]){P(19,9),P(22,12),P(19,15)}, 3, c, 0, th);
            dl->AddLine(P(2,12), P(22,12), c, th);
            dl->AddLine(P(12,2), P(12,22), c, th);
        } break;
        case ICO_ESP: {
            dl->AddCircle(P(12,7), sz * (4.f/24.f), c, 0, th);
            dl->AddBezierQuadratic(P(4,21), P(12,10), P(20,21), c, th);
            // dashed rect - approximate with 4 small segments per side
            float x0 = P(2,1.5f).x, y0 = P(2,1.5f).y;
            float x1 = P(22,22.5f).x, y1 = P(22,22.5f).y;
            for (int i = 0; i < 4; i++) {
                float t1 = i * 0.25f + 0.03f, t2 = (i+1) * 0.25f - 0.03f;
                dl->AddLine(ImVec2(x0 + (x1-x0)*t1, y0), ImVec2(x0 + (x1-x0)*t2, y0), c, th);
                dl->AddLine(ImVec2(x0 + (x1-x0)*t1, y1), ImVec2(x0 + (x1-x0)*t2, y1), c, th);
                dl->AddLine(ImVec2(x0, y0 + (y1-y0)*t1), ImVec2(x0, y0 + (y1-y0)*t2), c, th);
                dl->AddLine(ImVec2(x1, y0 + (y1-y0)*t1), ImVec2(x1, y0 + (y1-y0)*t2), c, th);
            }
        } break;
        case ICO_HUD: {
            dl->AddRect(P(3,3), P(21,21), c, 2.f, 0, th);
            dl->AddLine(P(3,9), P(21,9), c, th);
            dl->AddLine(P(9,21), P(9,9), c, th);
        } break;
        case ICO_WORLD: {
            dl->AddCircle(P(12,12), sz * (10.f/24.f), c, 0, th);
            dl->AddLine(P(2,12), P(22,12), c, th);
            dl->AddBezierQuadratic(P(12,2), P(6,12), P(12,22), c, th);
            dl->AddBezierQuadratic(P(12,2), P(18,12), P(12,22), c, th);
        } break;
        case ICO_MISC: {
            dl->AddLine(P(4,21), P(4,14), c, th);
            dl->AddLine(P(4,10), P(4,3), c, th);
            dl->AddLine(P(12,21), P(12,12), c, th);
            dl->AddLine(P(12,8), P(12,3), c, th);
            dl->AddLine(P(20,21), P(20,16), c, th);
            dl->AddLine(P(20,12), P(20,3), c, th);
            dl->AddLine(P(1,14), P(7,14), c, th);
            dl->AddLine(P(9,8), P(15,8), c, th);
            dl->AddLine(P(17,16), P(23,16), c, th);
        } break;
        case ICO_SCRIPTS: {
            dl->AddPolyline((const ImVec2[]){P(16,6),P(22,12),P(16,18)}, 3, c, 0, th);
            dl->AddPolyline((const ImVec2[]){P(8,6),P(2,12),P(8,18)}, 3, c, 0, th);
            dl->AddLine(P(14,4), P(10,20), c, th);
        } break;
    }
}

static bool SidebarBtn(ImDrawList* dl, float x, float y, float w, const char* label, SbIcon icon, bool sel) {
    float h = 34.f;
    ImVec2 tl(x, y), br(x + w, y + h);
    ImGuiWindow* wnd = ImGui::GetCurrentWindow();
    ImGuiID id = wnd->GetID(label);
    ImRect r(tl, br);
    ImGui::ItemAdd(r, id);
    bool hov, held;
    bool pressed = ImGui::ButtonBehavior(r, id, &hov, &held);
    if (sel) {
        dl->AddRectFilledMultiColor(tl, br, C::active_grad_a, C::active_grad_b, C::active_grad_b, C::active_grad_a);
        dl->AddRectFilled(ImVec2(tl.x, tl.y), ImVec2(tl.x + 2.f, br.y), C::accent);
    } else if (hov) {
        dl->AddRectFilled(tl, br, C::hover, 6.f);
    }
    float ix = tl.x + 18.f, iy = tl.y + h * 0.5f;
    DrawIcon(dl, ix, iy, 16.f, icon, sel ? C::accent : C::text_side);
    dl->AddText(ImVec2(tl.x + 36.f, tl.y + (h - ImGui::GetFontSize()) * 0.5f), sel ? C::text : C::text_side, label);
    return pressed;
}

// ---------- Menu ----------

static void DrawMenu(const ImGuiIO& io) {
    float W = 1080, H = 720;
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - W) * 0.5f, (io.DisplaySize.y - H) * 0.5f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(6/255.f, 9/255.f, 18/255.f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0x1d/255.f, 0x20/255.f, 0x29/255.f, 1.f));

    if (!ImGui::Begin("##spx", nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoScrollbar|
        ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove)) {
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float titleh = 44.f;
    float sb_w   = 210.f;

    // Title bar
    dl->AddRectFilled(wp, ImVec2(wp.x + W, wp.y + titleh), C::title_bg, 10.f, ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(wp.x, wp.y + titleh - 1), ImVec2(wp.x + W, wp.y + titleh - 1), C::soft_bd, 1.f);
    dl->AddText(ImVec2(wp.x + 22, wp.y + 15), C::accent, "SPAXER");
    dl->AddText(ImVec2(wp.x + 22 + ImGui::CalcTextSize("SPAXER").x + 4, wp.y + 15), C::text_dim, ".CS2");
    // Search box
    ImVec2 sb_tl(wp.x + 232, wp.y + 10);
    ImVec2 sb_br(wp.x + W - 60, wp.y + 34);
    dl->AddRectFilled(sb_tl, sb_br, C::root_bg, 6.f);
    dl->AddRect(sb_tl, sb_br, C::accent, 6.f, 0, 1.f);
    // magnify icon
    float ix = sb_tl.x + 12, iy = sb_tl.y + 12;
    dl->AddCircle(ImVec2(ix, iy), 5.f, C::text_dim, 12, 1.4f);
    dl->AddLine(ImVec2(ix + 3.5f, iy + 3.5f), ImVec2(ix + 8, iy + 8), C::text_dim, 1.4f);
    dl->AddText(ImVec2(sb_tl.x + 26, sb_tl.y + 5), C::text_dim, "Search settings...");
    // Close X
    ImVec2 xc(wp.x + W - 26, wp.y + 22);
    dl->AddLine(ImVec2(xc.x - 5, xc.y - 5), ImVec2(xc.x + 5, xc.y + 5), C::text_dim, 1.3f);
    dl->AddLine(ImVec2(xc.x - 5, xc.y + 5), ImVec2(xc.x + 5, xc.y - 5), C::text_dim, 1.3f);

    // Sidebar bg + separator
    dl->AddRectFilled(ImVec2(wp.x, wp.y + titleh), ImVec2(wp.x + sb_w, wp.y + H), C::title_bg, 10.f, ImDrawFlags_RoundCornersBottomLeft);
    dl->AddLine(ImVec2(wp.x + sb_w, wp.y + titleh), ImVec2(wp.x + sb_w, wp.y + H), C::soft_bd, 1.f);

    // Sidebar items (absolute positions inside window)
    struct Item { const char* group; const char* name; int idx; SbIcon icon; };
    Item items[] = {
        {"COMBAT",   "Legit bot", 0, ICO_LEGIT},
        {"MOVEMENT", "Movement",  1, ICO_MOVE},
        {"VISUALS",  "ESP",       2, ICO_ESP},
        {"VISUALS",  "HUD",       3, ICO_HUD},
        {"VISUALS",  "World",     4, ICO_WORLD},
        {"OTHER",    "Misc",      5, ICO_MISC},
        {"OTHER",    "Scripts",   6, ICO_SCRIPTS},
    };
    float y_cursor = wp.y + titleh + 14.f;
    const char* last_group = nullptr;
    for (auto& it : items) {
        if (!last_group || strcmp(last_group, it.group) != 0) {
            if (last_group) y_cursor += 6.f;
            SidebarLabel(dl, wp.x + 20, y_cursor, it.group);
            y_cursor += 22.f;
            last_group = it.group;
        }
        if (SidebarBtn(dl, wp.x + 10, y_cursor, sb_w - 20, it.name, it.icon, g_tab == it.idx)) g_tab = it.idx;
        y_cursor += 34.f;
    }

    // Configs card (bottom)
    float cfg_h = 118.f;
    float uc_h  = 52.f;
    float cfg_top = wp.y + H - 12.f - uc_h - 10.f - cfg_h;
    ImVec2 cfg_tl(wp.x + 12, cfg_top), cfg_br(wp.x + sb_w - 12, cfg_top + cfg_h);
    dl->AddRectFilled(cfg_tl, cfg_br, IM_COL32(12, 16, 28, 255), 8.f);
    dl->AddRect(cfg_tl, cfg_br, C::card_bd, 8.f, 0, 1.f);
    dl->AddText(ImVec2(cfg_tl.x + 12, cfg_tl.y + 10), C::sec_head, "CONFIGS");
    ImVec2 dd_tl(cfg_tl.x + 12, cfg_tl.y + 30), dd_br(cfg_br.x - 12, cfg_tl.y + 54);
    dl->AddRectFilled(dd_tl, dd_br, C::root_bg, 5.f);
    dl->AddRect(dd_tl, dd_br, C::root_bd, 5.f, 0, 1.f);
    dl->AddText(ImVec2(dd_tl.x + 10, dd_tl.y + 4), C::text, "Default");
    dl->AddTriangleFilled(ImVec2(dd_br.x - 16, dd_tl.y + 10), ImVec2(dd_br.x - 8, dd_tl.y + 10), ImVec2(dd_br.x - 12, dd_tl.y + 15), C::text_dim);
    ImVec2 en_tl(cfg_tl.x + 12, cfg_tl.y + 60), en_br(cfg_br.x - 12, cfg_tl.y + 84);
    dl->AddRectFilled(en_tl, en_br, C::root_bg, 5.f);
    dl->AddRect(en_tl, en_br, C::root_bd, 5.f, 0, 1.f);
    dl->AddText(ImVec2(en_tl.x + 10, en_tl.y + 4), C::text_dim, "name");
    float bw = (cfg_br.x - cfg_tl.x - 24 - 12) / 3.f;
    const char* bl[] = { "Save", "Load", "Del" };
    for (int i = 0; i < 3; i++) {
        ImVec2 btl(cfg_tl.x + 12 + i * (bw + 6), cfg_tl.y + 90);
        ImVec2 bbr(btl.x + bw, btl.y + 22);
        dl->AddRectFilled(btl, bbr, IM_COL32(0x17, 0x1a, 0x24, 255), 5.f);
        dl->AddRect(btl, bbr, IM_COL32(0x23, 0x27, 0x36, 255), 5.f, 0, 1.f);
        ImVec2 ts = ImGui::CalcTextSize(bl[i]);
        dl->AddText(ImVec2(btl.x + (bw - ts.x) * 0.5f, btl.y + (22 - ts.y) * 0.5f), C::text, bl[i]);
    }

    // User card (bottom)
    ImVec2 uc_tl(wp.x + 12, wp.y + H - 12 - uc_h), uc_br(wp.x + sb_w - 12, wp.y + H - 12);
    dl->AddRectFilled(uc_tl, uc_br, IM_COL32(12, 16, 28, 255), 8.f);
    dl->AddRect(uc_tl, uc_br, C::card_bd, 8.f, 0, 1.f);
    dl->AddCircleFilled(ImVec2(uc_tl.x + 22, uc_tl.y + 26), 14.f, C::field_bg);
    dl->AddCircle(ImVec2(uc_tl.x + 22, uc_tl.y + 26), 14.f, C::root_bd, 0, 1.f);
    static std::string user = GetUser();
    dl->AddText(ImVec2(uc_tl.x + 46, uc_tl.y + 12), C::text, user.c_str());
    dl->AddText(ImVec2(uc_tl.x + 46, uc_tl.y + 28), C::accent, "Internal");

    // Content area
    ImGui::SetCursorPos(ImVec2(sb_w + 1, titleh));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
    ImGui::BeginChild("##content", ImVec2(W - sb_w - 1, H - titleh), false);

    float avail = ImGui::GetContentRegionAvail().x;
    float col_w = (avail - 14.f) * 0.5f;

    ImGui::BeginChild("##col1", ImVec2(col_w, 0), false);
    switch (g_tab) {
        case 0: {
            Section("AIMBOT", [&](){
                ToggleRow("Enabled", &g_aimbot, "g");
                const char* keys[] = { "Always", "R-Click", "Shift" };
                static int aimkey = 0;
                ComboRow("Aim key", &aimkey, keys, 3);
                const char* prio[] = { "Crosshair", "Distance", "Health" };
                static int p = 0;
                ComboRow("Target priority", &p, prio, 3);
                ToggleRow("AimLock",      &g_esp_health);
                SliderRow("Switch delay (ms)", &g_switch_delay, 0, 500);
                ToggleRow("Through walls",&g_esp_name);
                SliderRow("FOV",   &g_fov,   0, 180, "°");
                SliderRow("Speed", &g_speed, 0, 100);
                SliderRow("Sensitivity", &g_sens, 0, 400);
                ToggleRow("Head",  &g_esp_health);
                ToggleRow("Neck",  &g_esp_skeleton);
                ToggleRow("Chest", &g_chams);
                ToggleRow("Pelvis",&g_esp_box);
            });
            Section("RCS", [&](){
                ToggleRow("Enabled",  &g_rcs);
                SliderRow("Strength", &g_rcs_strength, 0, 100);
            });
            break;
        }
        case 1: {
            Section("BUNNY HOP", [&](){
                ToggleRow("Enabled",  &g_bhop, "v");
                ToggleRow("Auto jump",&g_autojump);
                const char* jm[] = { "Space", "Scroll" }; static int j = 0;
                ComboRow("Jump method", &j, jm, 2);
            });
            Section("AUTO STRAFE", [&](){
                ToggleRow("Enabled", &g_autostrafe, "v");
                const char* sm[] = { "Follow mouse", "Perfect", "Hybrid" }; static int s = 0;
                ComboRow("Strafe mode", &s, sm, 3);
            });
            break;
        }
        case 2: {
            Section("ESP", [&](){
                ToggleRow("Enabled",       &g_esp_box, "bracketleft");
                ToggleRow("Enemies only",  &g_esp_enemies_only);
                ToggleRow("Box",           &g_esp_box);
                ToggleRow("Health bar",    &g_esp_health);
                ToggleRow("Name",          &g_esp_name);
                ToggleRow("Player weapon", &g_esp_weapon);
                ToggleRow("Ammo",          &g_esp_ammo);
                ToggleRow("Skeleton",      &g_esp_skeleton);
                ToggleRow("Head circle",   &g_esp_flags);
                ToggleRow("Flags",         &g_esp_flags);
                ToggleRow("Grenade prediction", &g_grenade_pred);
                ToggleRow("Thrown grenades",    &g_esp_flags);
                ToggleRow("Off-screen arrows",  &g_arrows);
                ToggleRow("Dropped items",      &g_dropped_items);
            });
            Section("SOUND ESP", [&](){ ToggleRow("Enabled", &g_sound_esp, "bind"); });
            break;
        }
        case 3: {
            Section("HUD", [&](){
                ToggleRow("Watermark",     &g_watermark,  "bind");
                ToggleRow("Bomb timer",    &g_bomb_timer, "bind");
                ToggleRow("Keybinds panel",&g_hotkeys_hud,"bind");
                ToggleRow("Spectators list",&g_spectators);
                ToggleRow("Media player",  &g_media);
                ToggleRow("Velocity graph",&g_media);
                ToggleRow("Keystrokes",    &g_keystrokes);
                ToggleRow("Notifications", &g_notif);
                ToggleRow("Hitmarker",     &g_hitmarker);
                ToggleRow("Radar",         &g_radar);
            });
            break;
        }
        case 4: {
            Section("WORLD", [&](){
                SliderRow("Brightness %", &g_brightness, 0, 200);
                ToggleRow("Night mode (internal)",   &g_night, "bind");
                SpxFINightMode(g_night);
                SliderRow("Night strength",&g_night_strength, 0, 100);
                ToggleRow("Night sky (stars, moon)", &g_esp_flags);
                ToggleRow("Ambient tint", &g_esp_flags);
            });
            Section("SATURATION", [&](){
                ToggleRow("Saturation", &g_saturation);
                SliderRow("Saturation %", &g_sat_pct, 0, 300);
            });
            break;
        }
        case 5: {
            Section("UNSAFE (memory writes)", [&](){
                ToggleRow("Thirdperson (internal)", &g_thirdperson);
                static bool _sa = false, _aa = false;
                ToggleRow("Silent Aim (internal)", &_sa);
                ToggleRow("Anti Aim (internal)",  &_aa);
                SpxFISilentAim(_sa);
                SpxFIThirdperson(g_thirdperson);
                SpxFIAntiAim(_aa);
                ToggleRow("No flash",    &g_noflash);
                ToggleRow("No smoke",    &g_esp_flags);
                ToggleRow("Smoke recolor",&g_smoke_recolor);
                SliderRow("Smoke strength %", &g_smoke_str, 0, 255);
            });
            Section("HIT SOUND", [&](){
                const char* h[] = { "Ding", "Metal", "Bell" }; static int idx = 0;
                ComboRow("Hit sound", &idx, h, 3);
                SliderRow("Volume", &g_hitvol, 0, 100);
                ToggleRow("Kills only", &g_esp_name);
            });
            break;
        }
        case 6: {
            Section("LUA SCRIPTS", [&](){ ImGui::TextColored(ImVec4(0.48f,0.5f,0.58f,1), "Coming soon"); ImGui::Dummy(ImVec2(0, 10)); });
            break;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(0, 14);
    ImGui::BeginChild("##col2", ImVec2(col_w, 0), false);
    switch (g_tab) {
        case 0:
            Section("TRIGGER BOT", [&](){
                ToggleRow("Enabled",   &g_triggerbot, "r");
                ToggleRow("Aim correction",&g_esp_box);
                ToggleRow("Crouch fire",&g_esp_skeleton);
                ToggleRow("Flash check",&g_esp_health);
                ToggleRow("Autowall",  &g_esp_name);
                ToggleRow("Auto stop", &g_bhop);
                SliderRow("Hitchance", &g_hitc, 0, 100);
                SliderRow("Min damage",&g_mind, 0, 100);
            });
            Section("HUMANIZATION", [&](){
                ToggleRow("Enabled",   &g_hum);
                SliderRow("Speed min", &g_smin, 0, 100);
                SliderRow("Speed max", &g_smax, 0, 100);
                SliderRow("Shake",     &g_shake, 0, 100);
                SliderRow("Release speed", &g_release, 0, 100);
            });
            break;
        case 1: {
            Section("FAST STOP", [&](){
                ToggleRow("Enabled", &g_esp_box, "i");
                const char* fm[] = { "In air", "On ground" }; static int f = 0;
                ComboRow("Fast stop mode", &f, fm, 2);
            });
            Section("LADDER", [&](){
                ToggleRow("Fast ladder", &g_esp_skeleton);
                ToggleRow("Ladder jump assist", &g_esp_health);
            });
            Section("EDGE", [&](){
                ToggleRow("Edge jump", &g_esp_name, "j");
                ToggleRow("Edge bug",  &g_chams,   "bind");
            });
            break;
        }
        case 2: {
            Section("GLOW", [&](){
                ToggleRow("Enabled",   &g_glow, "bind");
                ToggleRow("Teammates", &g_esp_skeleton);
            });
            break;
        }
        case 3: {
            Section("CROSSHAIR", [&](){
                ToggleRow("Custom crosshair", &g_crosshair_custom, "bind");
                ToggleRow("Dot",              &g_crosshair_dot);
                ToggleRow("Outline",          &g_crosshair_outline);
                SliderRow("Size",       &g_crosshair_size, 1, 30);
                SliderRow("Gap",        &g_crosshair_gap, 0, 30);
                SliderRow("Thickness",  &g_crosshair_thickness, 1, 5);
                ToggleRow("Sniper crosshair", &g_esp_skeleton);
            });
            break;
        }
        case 4: {
            Section("WEATHER", [&](){
                const char* w[] = { "Off", "Rain", "Snow" }; static int wi = 0;
                ComboRow("Effect", &wi, w, 3);
                SliderRow("Density", &g_wdensity, 0, 100);
            });
            break;
        }
        case 5: {
            Section("RADAR HACK", [&](){ ToggleRow("Enabled", &g_radar); });
            Section("MISC", [&](){
                ToggleRow("Edit HUD positions", &g_edit_hud);
                ImVec2 p = ImGui::GetCursorScreenPos();
                float av = ImGui::GetContentRegionAvail().x;
                ImDrawList* d = ImGui::GetWindowDrawList();
                d->AddText(ImVec2(p.x + 4, p.y + 6), C::text_row, "Toggle GUI (bind)");
                ImVec2 chip_tl(p.x + av - 90, p.y + 2);
                ImVec2 chip_br(chip_tl.x + 84, chip_tl.y + 22);
                d->AddRectFilled(chip_tl, chip_br, C::field_bg, 4.f);
                d->AddRect(chip_tl, chip_br, C::field_bd, 4.f, 0, 1.f);
                d->AddText(ImVec2(chip_tl.x + 22, chip_tl.y + 4), C::text, "RShift");
                ImGui::Dummy(ImVec2(0, 30));
            });
            break;
        }
        default: break;
    }
    ImGui::EndChild();

    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void ImguiRenderFrame(VkCommandBuffer cb, VkImageView view, uint32_t w, uint32_t h) {
    if (!g_ready.load()) return;
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    g_display_w.store((float)w);
    g_display_h.store((float)h);

    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    if (dt <= 0.0001f) dt = 1.0f / 60.0f;
    if (dt > 0.5f) dt = 0.5f;
    last = now;
    io.DeltaTime = dt;
    UpdateFps(dt);

    bool menu = SpaxerMenuOpen();
    io.MousePos     = ImVec2(SpaxerMouseX(), SpaxerMouseY());
    io.MouseDown[0] = SpaxerMouseL();
    io.MouseDown[1] = menu && SpaxerMouseR();
    io.MouseWheel   = menu ? SpaxerScrollTake() : 0.f;

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    ImDrawList* fg = ImGui::GetForegroundDrawList(); (void)fg;
    // Internal HUD/menu disabled — external overlay handles everything visual.
    // Internal only runs SilentAim/AntiAim/Thirdperson/NightMode via features_internal.
    // ALL internal HUD/menu disabled — external overlay handles visuals.
    // Internal only runs SilentAim/AntiAim/Thirdperson/NightMode via features_internal.
    (void)fg;
    (void)menu;

    ImGui::Render();

    static VkFramebuffer fb = VK_NULL_HANDLE;
    static VkImageView last_view = VK_NULL_HANDLE;
    static uint32_t last_w = 0, last_h = 0;
    if (fb == VK_NULL_HANDLE || view != last_view || w != last_w || h != last_h) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(g_dev, fb, nullptr);
        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = g_rp;
        fi.attachmentCount = 1;
        fi.pAttachments = &view;
        fi.width = w; fi.height = h; fi.layers = 1;
        if (vkCreateFramebuffer(g_dev, &fi, nullptr, &fb) != VK_SUCCESS) return;
        last_view = view; last_w = w; last_h = h;
    }
    VkRenderPassBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = g_rp;
    bi.framebuffer = fb;
    bi.renderArea.extent = {w, h};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    vkCmdEndRenderPass(cb);
}
