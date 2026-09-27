#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/uio.h>
#include <unistd.h>

class Process {
public:
    bool Attach(const char* process_name);
    bool IsAlive() const;

    uintptr_t ModuleBase(const char* name) const;

    template <typename T>
    T Read(uintptr_t address) const {
        T value{};
        if (address < 0x10000 || m_pid <= 0) return value;
        iovec local{&value, sizeof(T)};
        iovec remote{reinterpret_cast<void*>(address), sizeof(T)};
        if (process_vm_readv(m_pid, &local, 1, &remote, 1, 0) != static_cast<ssize_t>(sizeof(T)))
            return T{};
        return value;
    }

    bool ReadBytes(uintptr_t address, void* buf, size_t len) const {
        if (address < 0x10000 || m_pid <= 0 || !buf) return false;
        iovec local{buf, len};
        iovec remote{reinterpret_cast<void*>(address), len};
        return process_vm_readv(m_pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(len);
    }

    template <typename T>
    bool Write(uintptr_t address, const T& value) const {
        if (address < 0x10000 || m_pid <= 0) return false;
        iovec local{const_cast<T*>(&value), sizeof(T)};
        iovec remote{reinterpret_cast<void*>(address), sizeof(T)};
        return process_vm_writev(m_pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(sizeof(T));
    }

    std::string ReadString(uintptr_t address, size_t max_len = 128) const;

    int pid() const { return m_pid; }

private:
    int m_pid = -1;
    std::string m_name;
};

extern Process g_proc;
