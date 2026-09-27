#include "input.h"
#include "state.h"
#include <initializer_list>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <cstring>
#include <cstdio>

Input g_input;

static int s_mouse_fd = -1;
static int s_kbd_fd   = -1;
static bool s_mouse_created = false;
static bool s_kbd_created   = false;

static int CreateMouse() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    struct uinput_setup u{};
    u.id.bustype = BUS_USB;
    u.id.vendor = 0x046d;
    u.id.product = 0xc077;
    strncpy(u.name, "Logitech USB Optical Mouse", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(fd, UI_DEV_SETUP, &u) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static int CreateKbd() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTCTRL);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
    ioctl(fd, UI_SET_KEYBIT, KEY_A);
    ioctl(fd, UI_SET_KEYBIT, KEY_D);
    ioctl(fd, UI_SET_KEYBIT, KEY_SPACE);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    struct uinput_setup u{};
    u.id.bustype = BUS_USB;
    u.id.vendor = 0x413c;
    u.id.product = 0x2107;
    strncpy(u.name, "Dell KB216 Wired Keyboard", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(fd, UI_DEV_SETUP, &u) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd); return -1;
    }
    return fd;
}

void Input::Shutdown() {
    if (s_kbd_fd >= 0) {
        input_event ev{}; ev.type = EV_KEY;
        for (int code : {KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_A, KEY_D, KEY_SPACE}) {
            ev.code = code; ev.value = 0;
            write(s_kbd_fd, &ev, sizeof(ev));
        }
        input_event syn{}; syn.type = EV_SYN; syn.code = SYN_REPORT;
        write(s_kbd_fd, &syn, sizeof(syn));
    }
    m_crouch_held.store(false);
    if (s_mouse_fd >= 0) {
        if (s_mouse_created) ioctl(s_mouse_fd, UI_DEV_DESTROY);
        close(s_mouse_fd);
        s_mouse_fd = -1; s_mouse_created = false;
    }
    if (s_kbd_fd >= 0) {
        if (s_kbd_created) ioctl(s_kbd_fd, UI_DEV_DESTROY);
        close(s_kbd_fd);
        s_kbd_fd = -1; s_kbd_created = false;
    }
    for (int i = 0; i < m_keyboard_count; i++)
        close(m_keyboards[i]);
    m_keyboard_count = 0;
    for (int i = 0; i < m_mouse_count; i++)
        close(m_mice[i]);
    m_mouse_count = 0;
    if (m_display) {
        XCloseDisplay(static_cast<Display*>(m_display));
        m_display = nullptr;
    }
}

static void EmitTo(int fd, int type, int code, int value) {
    if (fd < 0) return;
    input_event ev{};
    ev.type = type; ev.code = code; ev.value = value;
    write(fd, &ev, sizeof(ev));
    input_event syn{};
    syn.type = EV_SYN; syn.code = SYN_REPORT;
    write(fd, &syn, sizeof(syn));
}
static void EmitKey(int code, int value) { EmitTo(s_kbd_fd,   EV_KEY, code, value); }
static void EmitBtn(int code, int value) { EmitTo(s_mouse_fd, EV_KEY, code, value); }

bool Input::Init() {
    Display* d = XOpenDisplay(nullptr);
    if (d) m_display = d;
    for (int i = 0; i < 32 && m_keyboard_count < 16; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned char event_bits[(EV_CNT + 7) / 8] = {};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0) {
            close(fd);
            continue;
        }
        bool has_key = (event_bits[EV_KEY / 8] & (1 << (EV_KEY % 8))) != 0;
        bool has_rel = (event_bits[EV_REL / 8] & (1 << (EV_REL % 8))) != 0;
        bool has_abs = (event_bits[EV_ABS / 8] & (1 << (EV_ABS % 8))) != 0;
        unsigned char key_bits[(KEY_CNT + 7) / 8] = {};
        if (has_key) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
        bool is_mouse = has_key && has_rel && (key_bits[BTN_LEFT / 8] & (1 << (BTN_LEFT % 8)));
        if (is_mouse && m_mouse_count < 8) {
            m_mice[m_mouse_count++] = fd;
            continue;
        }
        if (!has_key || has_rel || has_abs) {
            close(fd);
            continue;
        }
        bool is_kbd = (key_bits[KEY_A / 8] & (1 << (KEY_A % 8))) &&
                      (key_bits[KEY_SPACE / 8] & (1 << (KEY_SPACE % 8))) &&
                      (key_bits[KEY_ENTER / 8] & (1 << (KEY_ENTER % 8)));
        if (is_kbd)
            m_keyboards[m_keyboard_count++] = fd;
        else
            close(fd);
    }
    return d != nullptr || m_keyboard_count > 0;
}

static void EnsureMouse() {
    if (s_mouse_created && s_mouse_fd >= 0) return;
    int fd = CreateMouse();
    if (fd >= 0) { s_mouse_fd = fd; s_mouse_created = true; }
    else fprintf(stderr, "[input] uinput mouse unavailable\n");
}
static void EnsureKbd() {
    if (s_kbd_created && s_kbd_fd >= 0) return;
    int fd = CreateKbd();
    if (fd >= 0) { s_kbd_fd = fd; s_kbd_created = true; }
    else fprintf(stderr, "[input] uinput kbd unavailable\n");
}

bool Input::IsMouseDown(int button) const {
    int code = button == 1 ? BTN_LEFT : button == 2 ? BTN_MIDDLE : button == 3 ? BTN_RIGHT : button == 4 ? BTN_SIDE : button == 5 ? BTN_EXTRA : -1;
    if (code < 0) return false;
    unsigned char keys[(KEY_CNT + 7) / 8] = {};
    for (int i = 0; i < m_mouse_count; i++) {
        if (ioctl(m_mice[i], EVIOCGKEY(sizeof(keys)), keys) >= 0 && (keys[code / 8] & (1 << (code % 8))))
            return true;
    }
    return false;
}

bool Input::IsKeyDown(int key_code) const {
    if (key_code < 0 || key_code >= KEY_CNT) return false;
    unsigned char keys[(KEY_CNT + 7) / 8] = {};
    for (int i = 0; i < m_keyboard_count; i++) {
        if (ioctl(m_keyboards[i], EVIOCGKEY(sizeof(keys)), keys) >= 0 &&
            (keys[key_code / 8] & (1 << (key_code % 8))))
            return true;
    }
    if (m_display) {
        Display* d = static_cast<Display*>(m_display);
        char xkeys[32]{};
        XQueryKeymap(d, xkeys);
        KeySym sym = NoSymbol;
        switch (key_code) {
            case KEY_SPACE: sym = XK_space; break;
            case KEY_LEFTSHIFT: sym = XK_Shift_L; break;
            case KEY_RIGHTSHIFT: sym = XK_Shift_R; break;
            case KEY_LEFTCTRL: sym = XK_Control_L; break;
            case KEY_RIGHTCTRL: sym = XK_Control_R; break;
            case KEY_A: sym = XK_a; break;
            case KEY_B: sym = XK_b; break;
            case KEY_C: sym = XK_c; break;
            case KEY_D: sym = XK_d; break;
            case KEY_E: sym = XK_e; break;
            case KEY_F: sym = XK_f; break;
            case KEY_G: sym = XK_g; break;
            case KEY_H: sym = XK_h; break;
            case KEY_I: sym = XK_i; break;
            case KEY_J: sym = XK_j; break;
            case KEY_K: sym = XK_k; break;
            case KEY_L: sym = XK_l; break;
            case KEY_M: sym = XK_m; break;
            case KEY_N: sym = XK_n; break;
            case KEY_O: sym = XK_o; break;
            case KEY_P: sym = XK_p; break;
            case KEY_Q: sym = XK_q; break;
            case KEY_R: sym = XK_r; break;
            case KEY_S: sym = XK_s; break;
            case KEY_T: sym = XK_t; break;
            case KEY_U: sym = XK_u; break;
            case KEY_V: sym = XK_v; break;
            case KEY_W: sym = XK_w; break;
            case KEY_X: sym = XK_x; break;
            case KEY_Y: sym = XK_y; break;
            case KEY_Z: sym = XK_z; break;
            default: break;
        }
        if (sym != NoSymbol) {
            KeyCode kc = XKeysymToKeycode(d, sym);
            if (kc && (xkeys[kc / 8] & (1 << (kc & 7)))) return true;
        }
    }
    return false;
}

void Input::HoldCrouch(bool down) {
    if (down == m_crouch_held.load()) return;
    if (down) EnsureKbd();
    if (s_kbd_fd >= 0) {
        EmitKey(KEY_LEFTCTRL, down ? 1 : 0);
    } else if (m_display) {
        Display* d = static_cast<Display*>(m_display);
        KeyCode kc = XKeysymToKeycode(d, XK_Control_L);
        XTestFakeKeyEvent(d, kc, down ? True : False, 0);
        XFlush(d);
    }
    m_crouch_held.store(down);
}

void Input::SetSnapKey(int key_code, bool down, std::atomic<bool>& virt) {
    if (virt.exchange(down) == down) return;
    EnsureKbd();
    if (s_kbd_fd >= 0) {
        EmitKey(key_code, down ? 1 : 0);
        return;
    }
    Display* display = static_cast<Display*>(m_display);
    if (!display) return;
    KeyCode kc = XKeysymToKeycode(display, key_code == KEY_A ? XK_a : XK_d);
    if (!kc) return;
    XTestFakeKeyEvent(display, kc, down ? True : False, CurrentTime);
    XFlush(display);
}

void Input::SnapTap() {
    bool a = IsKeyDown(KEY_A);
    bool d_down = IsKeyDown(KEY_D);
    if (!g_hud.cs2_focused.load()) {
        m_snap_a.store(a);
        m_snap_d.store(d_down);
        m_snap_priority.store(0);
        SetSnapKey(KEY_A, false, m_snap_virt_a);
        SetSnapKey(KEY_D, false, m_snap_virt_d);
        return;
    }
    bool old_a = m_snap_a.exchange(a);
    bool old_d = m_snap_d.exchange(d_down);
    if (a && !old_a) m_snap_priority.store(1);
    else if (d_down && !old_d) m_snap_priority.store(-1);
    bool want_a = false;
    bool want_d = false;
    if (a && d_down) {
        if (m_snap_priority.load() < 0) want_d = true;
        else want_a = true;
    } else if (a) {
        want_a = true;
        m_snap_priority.store(1);
    } else if (d_down) {
        want_d = true;
        m_snap_priority.store(-1);
    } else {
        m_snap_priority.store(0);
    }
    SetSnapKey(KEY_A, want_a, m_snap_virt_a);
    SetSnapKey(KEY_D, want_d, m_snap_virt_d);
}

void Input::SetAutoStrafe(int direction) {
    direction = direction < 0 ? -1 : direction > 0 ? 1 : 0;
    int previous = m_strafe_direction.exchange(direction);
    if (previous == direction) return;
    EnsureKbd();
    if (s_kbd_fd >= 0) {
        if (previous < 0) EmitKey(KEY_A, 0);
        if (previous > 0) EmitKey(KEY_D, 0);
        if (direction < 0) EmitKey(KEY_A, 1);
        if (direction > 0) EmitKey(KEY_D, 1);
        return;
    }
    Display* display = static_cast<Display*>(m_display);
    if (!display) return;
    if (previous < 0) XTestFakeKeyEvent(display, XKeysymToKeycode(display, XK_a), False, CurrentTime);
    if (previous > 0) XTestFakeKeyEvent(display, XKeysymToKeycode(display, XK_d), False, CurrentTime);
    if (direction < 0) XTestFakeKeyEvent(display, XKeysymToKeycode(display, XK_a), True, CurrentTime);
    if (direction > 0) XTestFakeKeyEvent(display, XKeysymToKeycode(display, XK_d), True, CurrentTime);
    XFlush(display);
}

void Input::MouseMove(int dx, int dy) {
    if (m_display) {
        Display* d = static_cast<Display*>(m_display);
        XTestFakeRelativeMotionEvent(d, dx, dy, 0);
        XFlush(d);
        return;
    }
    EnsureMouse();
    if (s_mouse_fd < 0) return;
    input_event ex{}; ex.type = EV_REL; ex.code = REL_X; ex.value = dx;
    input_event ey{}; ey.type = EV_REL; ey.code = REL_Y; ey.value = dy;
    input_event syn{}; syn.type = EV_SYN; syn.code = SYN_REPORT;
    if (dx) write(s_mouse_fd, &ex, sizeof(ex));
    if (dy) write(s_mouse_fd, &ey, sizeof(ey));
    write(s_mouse_fd, &syn, sizeof(syn));
}

void Input::HoldShift(bool down) {
    EnsureKbd();
    if (s_kbd_fd >= 0) {
        EmitKey(KEY_LEFTSHIFT, down ? 1 : 0);
        return;
    }
    if (m_display) {
        Display* d = static_cast<Display*>(m_display);
        KeyCode kc = XKeysymToKeycode(d, XK_Shift_L);
        XTestFakeKeyEvent(d, kc, down ? True : False, 0);
        XFlush(d);
    }
}

void Input::ClickLeft() {
    if (m_display) {
        Display* d = static_cast<Display*>(m_display);
        XTestFakeButtonEvent(d, 1, True, 0);
        XFlush(d);
        usleep(8000);
        XTestFakeButtonEvent(d, 1, False, CurrentTime);
        XFlush(d);
        return;
    }
    EnsureMouse();
    if (s_mouse_fd >= 0) {
        EmitBtn(BTN_LEFT, 1);
        usleep(8000);
        EmitBtn(BTN_LEFT, 0);
    }
}

void Input::SetKey(int key_code, bool down) {
    if (key_code <= 0 || key_code >= KEY_CNT) return;
    EmitKey(key_code, down ? 1 : 0);
}

void Input::SetMouseButton(int button, bool down) {
    static constexpr unsigned int kXButtons[] = {0, 1, 2, 3, 8, 9};
    if (button < 1 || button > 5 || !m_display) return;
    Display* d = static_cast<Display*>(m_display);
    XTestFakeButtonEvent(d, kXButtons[button], down ? True : False, CurrentTime);
    XFlush(d);
}
