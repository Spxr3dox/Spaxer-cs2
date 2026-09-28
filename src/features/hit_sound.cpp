#include "features/hit_sound.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <vector>

extern char** environ;

namespace hitsound {

namespace {

constexpr int kSampleRate = 44100;
constexpr double kTwoPi = 6.283185307179586;

std::vector<float> Synthesize(Style style, bool kill) {
    auto tone = [](std::vector<float>& out, double start, double seconds, double freq, double decay, double gain,
                   double sweep_to = 0.0) {
        size_t first = static_cast<size_t>(start * kSampleRate);
        size_t count = static_cast<size_t>(seconds * kSampleRate);
        if (out.size() < first + count) out.resize(first + count, 0.f);
        double phase = 0.0;
        for (size_t i = 0; i < count; i++) {
            double t = static_cast<double>(i) / kSampleRate;
            double f = sweep_to > 0.0 ? freq + (sweep_to - freq) * (t / seconds) : freq;
            phase += kTwoPi * f / kSampleRate;
            double attack = std::min(1.0, t / 0.002);
            out[first + i] += static_cast<float>(std::sin(phase) * std::exp(-t * decay) * attack * gain);
        }
    };
    std::vector<float> samples;
    double pitch = kill ? 1.26 : 1.0;
    switch (style) {
        case Style::Click:
            tone(samples, 0.0, 0.05, 2400 * pitch, 90, 0.8);
            tone(samples, 0.0, 0.05, 4100 * pitch, 120, 0.3);
            break;
        case Style::Ding:
            tone(samples, 0.0, 0.35, 1760 * pitch, 11, 0.6);
            tone(samples, 0.0, 0.35, 2640 * pitch, 15, 0.25);
            break;
        case Style::Bell:
            tone(samples, 0.0, 0.6, 880 * pitch, 6, 0.5);
            tone(samples, 0.0, 0.6, 1320 * pitch, 8, 0.3);
            tone(samples, 0.0, 0.6, 2210 * pitch, 11, 0.2);
            break;
        case Style::Pop:
            tone(samples, 0.0, 0.09, 900 * pitch, 30, 0.8, 300 * pitch);
            break;
        case Style::Off:
            break;
    }
    if (kill && style != Style::Off) {
        std::vector<float> echo = samples;
        size_t delay = static_cast<size_t>(0.09 * kSampleRate);
        samples.resize(samples.size() + delay, 0.f);
        for (size_t i = 0; i < echo.size(); i++) samples[i + delay] += echo[i] * 0.8f;
    }
    return samples;
}

bool WriteWav(const std::string& path, const std::vector<float>& samples) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
    uint32_t riff_size = 36 + data_bytes, fmt_size = 16, byte_rate = kSampleRate * 2, sample_rate = kSampleRate;
    uint16_t format = 1, channels = 1, block_align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff_size, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size, 4, 1, f); fwrite(&format, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&block_align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    for (float sample : samples) {
        int16_t value = static_cast<int16_t>(std::clamp(sample, -1.f, 1.f) * 32000.f);
        fwrite(&value, 2, 1, f);
    }
    fclose(f);
    return true;
}

std::string SoundPath(Style style, bool kill) {
    const char* home = getenv("HOME");
    std::string dir = std::string(home ? home : "/tmp") + "/.config/spaxer/sounds";
    mkdir(dir.c_str(), 0755);
    std::string path = dir + "/hit" + std::to_string(static_cast<int>(style)) + (kill ? "_kill" : "") + ".wav";
    struct stat st;
    if (stat(path.c_str(), &st) != 0) WriteWav(path, Synthesize(style, kill));
    return path;
}

}

void Play(Style style, int volume_percent, bool kill) {
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    if (style == Style::Off) return;
    std::string path = SoundPath(style, kill);
    std::string volume = std::to_string(std::clamp(volume_percent, 0, 100) / 100.0);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    char* argv[] = {const_cast<char*>("pw-play"), const_cast<char*>("--volume"), volume.data(), path.data(), nullptr};
    pid_t child;
    posix_spawnp(&child, "pw-play", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
}

}
