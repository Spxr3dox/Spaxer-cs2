#pragma once
#include <atomic>

class Input {
public:
    bool Init();
    void Shutdown();
    bool IsMouseDown(int button) const;
    bool IsKeyDown(int key_code) const;
    bool IsPhysicalKeyDown(int key_code) const;
    int TakeMouseDX();
    void HoldCrouch(bool down);
    void SnapTap();
    void SetAutoStrafe(int direction);
    void SetVirtualKey(int key_code, bool down);
    void ForceVirtualKey(int key_code, bool down);
    void ClickLeft();
    void ClickRight();
    void MouseMove(int dx, int dy);
    void ScrollDown();
    void HoldShift(bool down);
    void SetKey(int key_code, bool down);
    void SetMouseButton(int button, bool down);

private:
    void* m_display = nullptr;
    int m_keyboards[16]{};
    int m_keyboard_count = 0;
    int m_mice[8]{};
    int m_mouse_count = 0;
    void SetSnapKey(int key_code, bool down, std::atomic<bool>& virt);
    std::atomic<bool> m_crouch_held{false};
    std::atomic<bool> m_snap_a{false};
    std::atomic<bool> m_snap_d{false};
    std::atomic<int> m_snap_priority{0};
    std::atomic<bool> m_snap_virt_a{false};
    std::atomic<bool> m_snap_virt_d{false};
    void FakeKey(int key_code, bool down);
    std::atomic<bool>* VirtualKeyState(int key_code);
    std::atomic<bool> m_virtual_space{false};
    std::atomic<bool> m_virtual_a{false};
    std::atomic<bool> m_virtual_d{false};
    std::atomic<bool> m_virtual_w{false};
    std::atomic<bool> m_virtual_s{false};
};

extern Input g_input;
