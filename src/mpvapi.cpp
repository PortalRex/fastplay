#include "mpvapi.h"
#include <vector>

MpvApi g_mpv;

bool MpvApi::Load(const std::wstring& dllPath) {
    HMODULE m = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return false;
    bool ok = true;
#define X(name)                                                                \
    name = reinterpret_cast<decltype(name)>(GetProcAddress(m, #name));          \
    if (!name) ok = false;
    MPV_FUNCS(X)
#undef X
    loaded = ok;
    return ok;
}

namespace mpvx {

static std::vector<const char*> Args(std::initializer_list<const char*> args) {
    std::vector<const char*> v(args);
    v.push_back(nullptr);
    return v;
}

void Cmd(mpv_handle* h, std::initializer_list<const char*> args) {
    if (!h) return;
    auto v = Args(args);
    g_mpv.mpv_command(h, v.data());
}

void CmdAsync(mpv_handle* h, std::initializer_list<const char*> args) {
    if (!h) return;
    auto v = Args(args);
    g_mpv.mpv_command_async(h, 0, v.data());
}

void SetStr(mpv_handle* h, const char* name, const char* value) {
    if (h) g_mpv.mpv_set_property_string(h, name, value);
}

void SetFlag(mpv_handle* h, const char* name, bool v) {
    if (!h) return;
    int f = v ? 1 : 0;
    g_mpv.mpv_set_property(h, name, MPV_FORMAT_FLAG, &f);
}

void SetDouble(mpv_handle* h, const char* name, double v) {
    if (h) g_mpv.mpv_set_property(h, name, MPV_FORMAT_DOUBLE, &v);
}

double GetDouble(mpv_handle* h, const char* name, double def) {
    double v = def;
    if (!h || g_mpv.mpv_get_property(h, name, MPV_FORMAT_DOUBLE, &v) < 0) return def;
    return v;
}

int64_t GetInt(mpv_handle* h, const char* name, int64_t def) {
    int64_t v = def;
    if (!h || g_mpv.mpv_get_property(h, name, MPV_FORMAT_INT64, &v) < 0) return def;
    return v;
}

bool GetFlag(mpv_handle* h, const char* name) {
    int v = 0;
    if (!h || g_mpv.mpv_get_property(h, name, MPV_FORMAT_FLAG, &v) < 0) return false;
    return v != 0;
}

std::string GetStr(mpv_handle* h, const char* name) {
    if (!h) return {};
    char* s = g_mpv.mpv_get_property_string(h, name);
    if (!s) return {};
    std::string r = s;
    g_mpv.mpv_free(s);
    return r;
}

void ShowText(mpv_handle* h, const std::wstring& text, int ms) {
    if (!h) return;
    std::string t = util::Utf8(text);
    std::string d = std::to_string(ms);
    CmdAsync(h, {"show-text", t.c_str(), d.c_str()});
}

}  // namespace mpvx
