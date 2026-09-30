#include "../core/core.h"
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <linux/input.h>

static std::atomic<bool> g_menu_open{false};
static std::atomic<int>  g_key_w{0}, g_key_a{0}, g_key_s{0}, g_key_d{0}, g_key_space{0}, g_key_shift{0}, g_key_ctrl{0}, g_key_alt{0};
static std::atomic<float> g_mx{0.f}, g_my{0.f};
static std::atomic<bool>  g_mb_l{false}, g_mb_r{false};
static std::atomic<float> g_scroll{0.f};
extern std::atomic<float> g_display_w, g_display_h;

extern "C" bool  SpaxerMenuOpen()    { return g_menu_open.load(); }
extern "C" bool  SpaxerKey(int k)    {
    switch (k) { case 0: return g_key_w; case 1: return g_key_a; case 2: return g_key_s;
                 case 3: return g_key_d; case 4: return g_key_space; case 5: return g_key_shift;
                 case 6: return g_key_ctrl; case 7: return g_key_alt; } return false;
}
extern "C" float SpaxerMouseX()      { return g_mx.load(); }
extern "C" float SpaxerMouseY()      { return g_my.load(); }
extern "C" bool  SpaxerMouseL()      { return g_mb_l.load(); }
extern "C" bool  SpaxerMouseR()      { return g_mb_r.load(); }
extern "C" float SpaxerScrollTake()  { float v = g_scroll.exchange(0.f); return v; }

struct DevInfo { int fd; std::string path; bool has_kbd; bool has_mouse; };
static std::vector<DevInfo> g_devs;

static bool HasBit(int fd, int type, int code) {
    unsigned long bits[(KEY_MAX + sizeof(long)*8 - 1) / (sizeof(long)*8)] = {};
    if (ioctl(fd, EVIOCGBIT(type, sizeof(bits)), bits) < 0) return false;
    return (bits[code / (sizeof(long)*8)] >> (code % (sizeof(long)*8))) & 1UL;
}

static void OpenDevices() {
    DIR* d = opendir("/dev/input");
    if (!d) return;
    while (auto* e = readdir(d)) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        std::string path = std::string("/dev/input/") + e->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        DevInfo di{fd, path, false, false};
        di.has_kbd = HasBit(fd, EV_KEY, KEY_A) && HasBit(fd, EV_KEY, KEY_SPACE);
        di.has_mouse = HasBit(fd, EV_REL, REL_X) && HasBit(fd, EV_KEY, BTN_LEFT);
        if (!di.has_kbd && !di.has_mouse) { close(fd); continue; }
        g_devs.push_back(di);
        Log("[input] %s kbd=%d mouse=%d", path.c_str(), di.has_kbd, di.has_mouse);
    }
    closedir(d);
}

static void SetGrab(bool on) {
    for (auto& d : g_devs) {
        (void)on; // no grab, mouse events read passively
    }
}

static void InputThread() {
    OpenDevices();
    if (g_devs.empty()) { Log("[input] no devices"); return; }
    std::vector<pollfd> pfds(g_devs.size());
    for (size_t i = 0; i < g_devs.size(); i++) { pfds[i].fd = g_devs[i].fd; pfds[i].events = POLLIN; }
    bool last_menu = false;

    while (true) {
        bool menu = g_menu_open.load();
        if (menu != last_menu) { SetGrab(menu); last_menu = menu; }
        int r = poll(pfds.data(), pfds.size(), 50);
        if (r <= 0) continue;
        for (size_t i = 0; i < pfds.size(); i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            input_event ev;
            while (read(pfds[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_KEY) {
                    switch (ev.code) {
                        case KEY_RIGHTSHIFT: if (ev.value == 1) {
                            bool now = !g_menu_open.load();
                            g_menu_open.store(now);
                            if (now) {
                                float w = g_display_w.load(), h = g_display_h.load();
                                g_mx.store(w * 0.5f);
                                g_my.store(h * 0.5f);
                            }
                        } break;
                        case KEY_W: g_key_w.store(ev.value); break;
                        case KEY_A: g_key_a.store(ev.value); break;
                        case KEY_S: g_key_s.store(ev.value); break;
                        case KEY_D: g_key_d.store(ev.value); break;
                        case KEY_SPACE: g_key_space.store(ev.value); break;
                        case KEY_LEFTSHIFT: g_key_shift.store(ev.value); break;
                        case KEY_LEFTCTRL: g_key_ctrl.store(ev.value); break;
                        case KEY_LEFTALT: g_key_alt.store(ev.value); break;
                        case BTN_LEFT: g_mb_l.store(ev.value != 0); break;
                        case BTN_RIGHT: g_mb_r.store(ev.value != 0); break;
                        default: break;
                    }
                } else if (ev.type == EV_REL) {
                    float w = g_display_w.load(), h = g_display_h.load();
                    if (w < 100) w = 1920; if (h < 100) h = 1080;
                    if (ev.code == REL_X) g_mx.store(std::max(0.f, std::min(w, g_mx.load() + (float)ev.value)));
                    else if (ev.code == REL_Y) g_my.store(std::max(0.f, std::min(h, g_my.load() + (float)ev.value)));
                    else if (ev.code == REL_WHEEL) g_scroll.store(g_scroll.load() + (float)ev.value);
                }
            }
        }
    }
}

__attribute__((constructor(200)))
static void StartInput() {
    std::thread(InputThread).detach();
}
