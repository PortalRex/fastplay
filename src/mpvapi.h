// libmpv loaded at run time (LoadLibrary), so the window can appear before the 120 MB DLL is mapped.
#pragma once
#include "util.h"
#include <mpv/client.h>
#include <mpv/render.h>

#define MPV_FUNCS(X)                  \
    X(mpv_create)                     \
    X(mpv_initialize)                 \
    X(mpv_terminate_destroy)          \
    X(mpv_set_option)                 \
    X(mpv_set_option_string)          \
    X(mpv_set_property)               \
    X(mpv_set_property_string)        \
    X(mpv_set_property_async)         \
    X(mpv_get_property)               \
    X(mpv_get_property_string)        \
    X(mpv_observe_property)           \
    X(mpv_unobserve_property)         \
    X(mpv_command)                    \
    X(mpv_command_async)              \
    X(mpv_wait_event)                 \
    X(mpv_wakeup)                     \
    X(mpv_set_wakeup_callback)        \
    X(mpv_free)                       \
    X(mpv_free_node_contents)         \
    X(mpv_error_string)               \
    X(mpv_render_context_create)      \
    X(mpv_render_context_set_update_callback) \
    X(mpv_render_context_update)      \
    X(mpv_render_context_render)      \
    X(mpv_render_context_free)

struct MpvApi {
#define X(name) decltype(&::name) name = nullptr;
    MPV_FUNCS(X)
#undef X
    bool loaded = false;
    bool Load(const std::wstring& dllPath);
};

extern MpvApi g_mpv;

// Convenience wrappers on top of the loaded API.
namespace mpvx {
void Cmd(mpv_handle* h, std::initializer_list<const char*> args);       // synchronous
void CmdAsync(mpv_handle* h, std::initializer_list<const char*> args);  // fire and forget
void SetStr(mpv_handle* h, const char* name, const char* value);
void SetFlag(mpv_handle* h, const char* name, bool v);
void SetDouble(mpv_handle* h, const char* name, double v);
double GetDouble(mpv_handle* h, const char* name, double def = 0);
int64_t GetInt(mpv_handle* h, const char* name, int64_t def = 0);
bool GetFlag(mpv_handle* h, const char* name);
std::string GetStr(mpv_handle* h, const char* name);
void ShowText(mpv_handle* h, const std::wstring& text, int ms = 1200);
}  // namespace mpvx
