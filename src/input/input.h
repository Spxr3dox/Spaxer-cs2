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
    void ClickLeft();
    void MouseMove(int dx, int dy);
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
    std::atomic<int> m_strafe_direction{0};
};

extern Input g_input;
