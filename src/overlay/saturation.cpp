#include "overlay/saturation.h"
#include "config/settings.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>

extern char** environ;

namespace saturation {

namespace {

int s_applied = 100;
uint32_t s_applied_tint = 0;
int s_slot = 0;

std::string ShaderPath(int slot) {
    const char* home = getenv("HOME");
    std::string dir = std::string(home ? home : "/tmp") + "/.config/spaxer/shaders";
    mkdir(dir.c_str(), 0755);
    return dir + "/saturation_" + std::to_string(slot) + ".frag";
}

std::string GlslFloat(double value) {
    char text[32];
    snprintf(text, sizeof(text), "%.4f", value);
    for (char* p = text; *p; ++p) if (*p == ',') *p = '.';
    return text;
}

bool WriteShader(const std::string& path, double amount, uint32_t tint) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    std::string tint_rgb = GlslFloat(((tint >> 24) & 0xFF) / 255.0) + ", " + GlslFloat(((tint >> 16) & 0xFF) / 255.0) + ", " +
                           GlslFloat(((tint >> 8) & 0xFF) / 255.0);
    fprintf(f,
            "#version 300 es\n"
            "precision highp float;\n"
            "in vec2 v_texcoord;\n"
            "uniform sampler2D tex;\n"
            "out vec4 fragColor;\n"
            "void main() {\n"
            "    vec4 c = texture(tex, v_texcoord);\n"
            "    float l = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
            "    vec3 color = clamp(mix(vec3(l), c.rgb, %s), 0.0, 1.0);\n"
            "    color = mix(color, vec3(%s), %s);\n"
            "    fragColor = vec4(color, c.a);\n"
            "}\n",
            GlslFloat(amount).c_str(), tint_rgb.c_str(), GlslFloat((tint & 0xFF) / 255.0).c_str());
    fclose(f);
    return true;
}

void SetShader(const std::string& path, bool wait) {
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    std::string code = "hl.config({ decoration = { screen_shader = \"" + path + "\" } })";
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    char* argv[] = {const_cast<char*>("hyprctl"), const_cast<char*>("eval"), code.data(), nullptr};
    pid_t child = 0;
    if (posix_spawnp(&child, "hyprctl", &actions, nullptr, argv, environ) == 0 && wait) waitpid(child, nullptr, 0);
    posix_spawn_file_actions_destroy(&actions);
}

}

void Update(const Settings& settings, bool cs2_focused) {
    int wanted = settings::Enabled(settings.saturation) && cs2_focused ? settings.saturation_value : 100;
    if (wanted < 0) wanted = 0;
    if (wanted > 300) wanted = 300;
    uint32_t tint = cs2_focused && settings::Enabled(settings.night_mode) && settings::Enabled(settings.ambient_tint) ? settings.ambient_tint_rgba : 0u;
    if ((tint & 0xFF) == 0) tint = 0;
    if (wanted == s_applied && tint == s_applied_tint) return;
    s_applied = wanted;
    s_applied_tint = tint;
    if (wanted == 100 && tint == 0) {
        SetShader("", false);
        return;
    }
    s_slot ^= 1;
    std::string path = ShaderPath(s_slot);
    if (WriteShader(path, wanted / 100.0, tint)) SetShader(path, false);
}

void Reset() {
    if (s_applied == 100 && s_applied_tint == 0) return;
    s_applied = 100;
    s_applied_tint = 0;
    SetShader("", true);
}

}
