#include "overlay/crosshair_capture.h"
#include "features/features.h"
#include "overlay/wl/ext-foreign-toplevel-list-v1.h"
#include "overlay/wl/ext-image-capture-source-v1.h"
#include "overlay/wl/ext-image-copy-capture-v1.h"
#include <atomic>
#include <cairo.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace xhair {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kHalf = 64;
constexpr int kSize = kHalf * 2 + 1;
constexpr auto kIdleAfter = std::chrono::milliseconds(400);
constexpr auto kFreshFor = std::chrono::milliseconds(150);
constexpr auto kCaptureInterval = std::chrono::milliseconds(8);

std::atomic<bool> s_running{false};
std::atomic<int64_t> s_requested_ns{0};
std::thread s_thread;

std::mutex s_patch_mtx;
std::vector<uint32_t> s_patch(kSize * kSize, 0u);
Clock::time_point s_patch_time{};
bool s_patch_valid = false;

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

struct Toplevel {
    ext_foreign_toplevel_handle_v1* handle = nullptr;
    std::string app_id;
    bool closed = false;
};

struct Capture {
    wl_display* display = nullptr;
    wl_shm* shm = nullptr;
    ext_foreign_toplevel_list_v1* list = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* sources = nullptr;
    ext_image_copy_capture_manager_v1* copier = nullptr;
    std::vector<Toplevel*> toplevels;

    ext_image_capture_source_v1* source = nullptr;
    ext_image_copy_capture_session_v1* session = nullptr;
    Toplevel* target = nullptr;
    uint32_t width = 0, height = 0, format = 0;
    bool have_format = false, constraints_done = false, stopped = false;

    wl_buffer* buffer = nullptr;
    void* pixels = nullptr;
    size_t pixels_size = 0;
    uint32_t buffer_width = 0, buffer_height = 0;

    int frame_state = 0;
};

void HandleClosed(void* data, ext_foreign_toplevel_handle_v1*) { static_cast<Toplevel*>(data)->closed = true; }
void HandleDone(void*, ext_foreign_toplevel_handle_v1*) {}
void HandleTitle(void*, ext_foreign_toplevel_handle_v1*, const char*) {}
void HandleAppId(void* data, ext_foreign_toplevel_handle_v1*, const char* app_id) { static_cast<Toplevel*>(data)->app_id = app_id ? app_id : ""; }
void HandleIdentifier(void*, ext_foreign_toplevel_handle_v1*, const char*) {}
const ext_foreign_toplevel_handle_v1_listener kHandleListener = {HandleClosed, HandleDone, HandleTitle, HandleAppId, HandleIdentifier};

void ListToplevel(void* data, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* handle) {
    auto* capture = static_cast<Capture*>(data);
    auto* toplevel = new Toplevel;
    toplevel->handle = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &kHandleListener, toplevel);
    capture->toplevels.push_back(toplevel);
}
void ListFinished(void*, ext_foreign_toplevel_list_v1*) {}
const ext_foreign_toplevel_list_v1_listener kListListener = {ListToplevel, ListFinished};

void RegistryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
    auto* capture = static_cast<Capture*>(data);
    if (!strcmp(interface, wl_shm_interface.name))
        capture->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (!strcmp(interface, ext_foreign_toplevel_list_v1_interface.name))
        capture->list = static_cast<ext_foreign_toplevel_list_v1*>(wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1));
    else if (!strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name))
        capture->sources = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
            wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
    else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name))
        capture->copier = static_cast<ext_image_copy_capture_manager_v1*>(wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
}
void RegistryRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener = {RegistryGlobal, RegistryRemove};

void SessionBufferSize(void* data, ext_image_copy_capture_session_v1*, uint32_t width, uint32_t height) {
    auto* capture = static_cast<Capture*>(data);
    capture->width = width;
    capture->height = height;
}
void SessionShmFormat(void* data, ext_image_copy_capture_session_v1*, uint32_t format) {
    auto* capture = static_cast<Capture*>(data);
    if (format != WL_SHM_FORMAT_XRGB8888 && format != WL_SHM_FORMAT_ARGB8888) return;
    if (!capture->have_format || format == WL_SHM_FORMAT_XRGB8888) capture->format = format;
    capture->have_format = true;
}
void SessionDmabufDevice(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
void SessionDmabufFormat(void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {}
void SessionDone(void* data, ext_image_copy_capture_session_v1*) { static_cast<Capture*>(data)->constraints_done = true; }
void SessionStopped(void* data, ext_image_copy_capture_session_v1*) { static_cast<Capture*>(data)->stopped = true; }
const ext_image_copy_capture_session_v1_listener kSessionListener = {
    SessionBufferSize, SessionShmFormat, SessionDmabufDevice, SessionDmabufFormat, SessionDone, SessionStopped};

void FrameTransform(void*, ext_image_copy_capture_frame_v1*, uint32_t) {}
void FrameDamage(void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {}
void FramePresentation(void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {}
void FrameReady(void* data, ext_image_copy_capture_frame_v1*) { static_cast<Capture*>(data)->frame_state = 1; }
void FrameFailed(void* data, ext_image_copy_capture_frame_v1*, uint32_t) { static_cast<Capture*>(data)->frame_state = -1; }
const ext_image_copy_capture_frame_v1_listener kFrameListener = {FrameTransform, FrameDamage, FramePresentation, FrameReady, FrameFailed};

void DestroyBuffer(Capture& c) {
    if (c.buffer) wl_buffer_destroy(c.buffer);
    if (c.pixels) munmap(c.pixels, c.pixels_size);
    c.buffer = nullptr;
    c.pixels = nullptr;
    c.pixels_size = 0;
}

void DestroySession(Capture& c) {
    DestroyBuffer(c);
    if (c.session) ext_image_copy_capture_session_v1_destroy(c.session);
    if (c.source) ext_image_capture_source_v1_destroy(c.source);
    c.session = nullptr;
    c.source = nullptr;
    c.target = nullptr;
    c.constraints_done = c.have_format = c.stopped = false;
}

bool CreateBuffer(Capture& c) {
    DestroyBuffer(c);
    size_t stride = static_cast<size_t>(c.width) * 4;
    c.pixels_size = stride * c.height;
    int fd = memfd_create("spaxer-xhair", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, static_cast<off_t>(c.pixels_size)) != 0) { close(fd); return false; }
    c.pixels = mmap(nullptr, c.pixels_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (c.pixels == MAP_FAILED) { c.pixels = nullptr; close(fd); return false; }
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, static_cast<int32_t>(c.pixels_size));
    c.buffer = wl_shm_pool_create_buffer(pool, 0, static_cast<int32_t>(c.width), static_cast<int32_t>(c.height),
                                         static_cast<int32_t>(stride), c.format);
    wl_shm_pool_destroy(pool);
    close(fd);
    c.buffer_width = c.width;
    c.buffer_height = c.height;
    return c.buffer != nullptr;
}

bool Dispatch(Capture& c, int timeout_ms) {
    while (wl_display_prepare_read(c.display) != 0) wl_display_dispatch_pending(c.display);
    wl_display_flush(c.display);
    pollfd pfd{wl_display_get_fd(c.display), POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0) {
        wl_display_cancel_read(c.display);
        return wl_display_get_error(c.display) == 0;
    }
    if (wl_display_read_events(c.display) != 0) return false;
    return wl_display_dispatch_pending(c.display) >= 0;
}

Toplevel* FindGame(Capture& c) {
    for (Toplevel* toplevel : c.toplevels)
        if (!toplevel->closed && toplevel->app_id == "cs2") return toplevel;
    return nullptr;
}

bool Matches(uint32_t pixel, int r, int g, int b) {
    int pr = (pixel >> 16) & 0xFF, pg = (pixel >> 8) & 0xFF, pb = pixel & 0xFF;
    return std::abs(pr - r) < 72 && std::abs(pg - g) < 72 && std::abs(pb - b) < 72;
}

void BuildPatch(const Capture& c) {
    double cr, cg, cb;
    if (!features::GameCrosshairColor(cr, cg, cb)) return;
    int r = static_cast<int>(cr * 255), g = static_cast<int>(cg * 255), b = static_cast<int>(cb * 255);
    int x0 = static_cast<int>(c.buffer_width / 2) - kHalf, y0 = static_cast<int>(c.buffer_height / 2) - kHalf;
    if (x0 < 1 || y0 < 1 || x0 + kSize + 1 >= static_cast<int>(c.buffer_width) || y0 + kSize + 1 >= static_cast<int>(c.buffer_height)) return;
    const uint32_t* src = static_cast<const uint32_t*>(c.pixels);
    auto at = [&](int x, int y) { return src[static_cast<size_t>(y0 + y) * c.buffer_width + (x0 + x)]; };
    std::vector<uint8_t> mark(kSize * kSize, 0);
    for (int y = 0; y < kSize; y++)
        for (int x = 0; x < kSize; x++)
            if (Matches(at(x, y), r, g, b)) mark[y * kSize + x] = 1;
    std::vector<uint32_t> patch(kSize * kSize, 0u);
    for (int y = 0; y < kSize; y++) {
        for (int x = 0; x < kSize; x++) {
            uint32_t pixel = at(x, y);
            bool keep = mark[y * kSize + x];
            if (!keep) {
                int lum = (((pixel >> 16) & 0xFF) * 3 + ((pixel >> 8) & 0xFF) * 6 + (pixel & 0xFF)) / 10;
                if (lum < 70) {
                    for (int dy = -1; dy <= 1 && !keep; dy++)
                        for (int dx = -1; dx <= 1 && !keep; dx++) {
                            int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < kSize && ny < kSize && mark[ny * kSize + nx]) keep = true;
                        }
                }
            }
            if (keep) patch[y * kSize + x] = 0xFF000000u | (pixel & 0x00FFFFFFu);
        }
    }
    std::lock_guard<std::mutex> lock(s_patch_mtx);
    s_patch.swap(patch);
    s_patch_time = Clock::now();
    s_patch_valid = true;
}

void Run() {
    Capture c;
    c.display = wl_display_connect(nullptr);
    if (!c.display) return;
    wl_registry* registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(registry, &kRegistryListener, &c);
    wl_display_roundtrip(c.display);
    if (!c.shm || !c.list || !c.sources || !c.copier) {
        wl_display_disconnect(c.display);
        return;
    }
    ext_foreign_toplevel_list_v1_add_listener(c.list, &kListListener, &c);
    wl_display_roundtrip(c.display);
    wl_display_roundtrip(c.display);

    auto last_capture = Clock::now() - kCaptureInterval;
    while (s_running.load()) {
        if (!Dispatch(c, 0)) break;
        bool wanted = NowNs() - s_requested_ns.load() < std::chrono::duration_cast<std::chrono::nanoseconds>(kIdleAfter).count();
        if (c.target && c.target->closed) DestroySession(c);
        if (!wanted) {
            if (c.session) DestroySession(c);
            Dispatch(c, 50);
            continue;
        }
        if (!c.session) {
            Toplevel* game = FindGame(c);
            if (!game) { Dispatch(c, 200); continue; }
            c.target = game;
            c.source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(c.sources, game->handle);
            c.session = ext_image_copy_capture_manager_v1_create_session(c.copier, c.source, 0);
            ext_image_copy_capture_session_v1_add_listener(c.session, &kSessionListener, &c);
            auto deadline = Clock::now() + std::chrono::milliseconds(500);
            while (!c.constraints_done && !c.stopped && Clock::now() < deadline) Dispatch(c, 20);
            if (!c.constraints_done || !c.have_format || c.stopped) { DestroySession(c); Dispatch(c, 200); continue; }
        }
        if (c.stopped) { DestroySession(c); continue; }
        if (!c.buffer || c.buffer_width != c.width || c.buffer_height != c.height) {
            if (!CreateBuffer(c)) { DestroySession(c); Dispatch(c, 200); continue; }
        }
        auto now = Clock::now();
        if (now - last_capture < kCaptureInterval) {
            std::this_thread::sleep_for(kCaptureInterval - (now - last_capture));
        }
        last_capture = Clock::now();
        ext_image_copy_capture_frame_v1* frame = ext_image_copy_capture_session_v1_create_frame(c.session);
        ext_image_copy_capture_frame_v1_add_listener(frame, &kFrameListener, &c);
        ext_image_copy_capture_frame_v1_attach_buffer(frame, c.buffer);
        ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, static_cast<int32_t>(c.width), static_cast<int32_t>(c.height));
        ext_image_copy_capture_frame_v1_capture(frame);
        c.frame_state = 0;
        auto deadline = Clock::now() + std::chrono::milliseconds(200);
        while (c.frame_state == 0 && s_running.load() && Clock::now() < deadline)
            if (!Dispatch(c, 20)) break;
        ext_image_copy_capture_frame_v1_destroy(frame);
        if (c.frame_state == 1) BuildPatch(c);
        else if (c.frame_state == -1) { DestroySession(c); Dispatch(c, 100); }
    }
    DestroySession(c);
    for (Toplevel* toplevel : c.toplevels) {
        ext_foreign_toplevel_handle_v1_destroy(toplevel->handle);
        delete toplevel;
    }
    wl_display_disconnect(c.display);
}

}

void Start() {
    if (s_running.exchange(true)) return;
    s_thread = std::thread(Run);
}

void Stop() {
    if (!s_running.exchange(false)) return;
    if (s_thread.joinable()) s_thread.join();
}

void Request() {
    s_requested_ns.store(NowNs());
}

void Draw(cairo_t* cr, int width, int height) {
    std::vector<uint32_t> patch;
    {
        std::lock_guard<std::mutex> lock(s_patch_mtx);
        if (!s_patch_valid || Clock::now() - s_patch_time > kFreshFor) return;
        patch = s_patch;
    }
    cairo_surface_t* surface = cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(patch.data()),
                                                                   CAIRO_FORMAT_ARGB32, kSize, kSize, kSize * 4);
    cairo_save(cr);
    cairo_set_source_surface(cr, surface, width / 2 - kHalf, height / 2 - kHalf);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(surface);
}

}
