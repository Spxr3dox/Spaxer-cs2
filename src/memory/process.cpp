#include "process.h"
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

Process g_proc;

static int FindPidByName(const char* name) {
    DIR* d = opendir("/proc");
    if (!d) return -1;
    int found = -1;
    dirent* e;
    while ((e = readdir(d))) {
        if (e->d_type != DT_DIR) continue;
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char comm[256] = {};
        fgets(comm, sizeof(comm), f);
        fclose(f);
        size_t n = strlen(comm);
        if (n && comm[n - 1] == '\n') comm[n - 1] = 0;
        if (strcmp(comm, name) == 0) { found = pid; break; }
    }
    closedir(d);
    return found;
}

bool Process::Attach(const char* process_name) {
    m_pid = FindPidByName(process_name);
    m_name = process_name ? process_name : "";
    return m_pid > 0;
}

bool Process::IsAlive() const {
    if (m_pid <= 0) return false;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", m_pid);
    struct stat st;
    return stat(path, &st) == 0;
}

uintptr_t Process::ModuleBase(const char* name) const {
    if (m_pid <= 0 || !name) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", m_pid);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, name)) continue;
        if (!strstr(line, "r-xp") && !strstr(line, "r--p")) continue;
        uintptr_t start = 0;
        if (sscanf(line, "%lx-", &start) == 1) { base = start; break; }
    }
    fclose(f);
    return base;
}

std::string Process::ReadString(uintptr_t address, size_t max_len) const {
    if (max_len == 0 || max_len > 4096) max_len = 128;
    std::string out;
    out.resize(max_len);
    if (!ReadBytes(address, out.data(), max_len)) return {};
    out.resize(strnlen(out.c_str(), max_len));
    return out;
}
