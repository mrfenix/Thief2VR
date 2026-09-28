#include "settings.h"

#include "../log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const char kSection[] = "Thief2VR";

const char* IniPath()
{
    static char path[MAX_PATH];
    if (!path[0]) {
        DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        while (n > 0 && path[n - 1] != '\\')
            --n;
        lstrcpyA(path + n, "thief2vr.ini");
    }
    return path;
}

void Read(const char* name, bool& value, float lo, float hi)
{
    char buf[32];
    if (GetPrivateProfileStringA(kSection, name, "", buf, sizeof(buf), IniPath()) > 0)
        value = atoi(buf) != 0;
}

void Read(const char* name, float& value, float lo, float hi)
{
    char buf[32];
    if (GetPrivateProfileStringA(kSection, name, "", buf, sizeof(buf), IniPath()) > 0) {
        float v = (float)atof(buf);
        value = v < lo ? lo : v > hi ? hi : v;
    }
}

void Read(const char* name, int& value, float lo, float hi)
{
    char buf[32];
    if (GetPrivateProfileStringA(kSection, name, "", buf, sizeof(buf), IniPath()) > 0) {
        int v = atoi(buf);
        value = v < (int)lo ? (int)lo : v > (int)hi ? (int)hi : v;
    }
}

void Write(const char* name, bool value)
{
    WritePrivateProfileStringA(kSection, name, value ? "1" : "0", IniPath());
}

void Write(const char* name, float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4g", value);
    WritePrivateProfileStringA(kSection, name, buf, IniPath());
}

void Write(const char* name, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(kSection, name, buf, IniPath());
}

} // namespace

Settings& Config()
{
    static Settings settings;
    return settings;
}

void LoadSettings()
{
    Settings& s = Config();
#define SETTINGS_READ(type, name, def, lo, hi, tab, label) Read(#name, s.name, (float)(lo), (float)(hi));
    SETTINGS_LIST(SETTINGS_READ)
#undef SETTINGS_READ
    // Write back so the file always lists every setting with its current value.
    SaveSettings();
    Log("Settings loaded from %s", IniPath());
}

namespace {
constexpr SettingType TypeOf(bool*) { return SettingType::Bool; }
constexpr SettingType TypeOf(float*) { return SettingType::Float; }
constexpr SettingType TypeOf(int*) { return SettingType::Int; }
} // namespace

const SettingInfo* AllSettings(int& count)
{
    static SettingInfo infos[] = {
#define SETTINGS_INFO(type, name, def, lo, hi, tab, label) \
    {#name, label, tab, TypeOf((type*)nullptr), &Config().name, (float)(lo), (float)(hi), (float)(def)},
        SETTINGS_LIST(SETTINGS_INFO)
#undef SETTINGS_INFO
    };
    count = (int)(sizeof(infos) / sizeof(infos[0]));
    return infos;
}

void ResetSettingsTab(const char* tab)
{
    int count;
    const SettingInfo* infos = AllSettings(count);
    for (int i = 0; i < count; ++i) {
        const SettingInfo& s = infos[i];
        if (strcmp(s.tab, tab) != 0)
            continue;
        switch (s.type) {
        case SettingType::Bool: *static_cast<bool*>(s.value) = s.def != 0; break;
        case SettingType::Float: *static_cast<float*>(s.value) = s.def; break;
        case SettingType::Int: *static_cast<int*>(s.value) = (int)s.def; break;
        }
    }
}

void SaveSettings()
{
    const Settings& s = Config();
#define SETTINGS_WRITE(type, name, def, lo, hi, tab, label) Write(#name, s.name);
    SETTINGS_LIST(SETTINGS_WRITE)
#undef SETTINGS_WRITE
}
