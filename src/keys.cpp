#include "keys.h"
#include "settings.h"
#include <functional>
#include <cwctype>

// Defaults mirror MPC-HC's (clsid2/mpc-hc) default key map where it has one.
static const ActionInfo kActions[] = {
    {Act::PlayPause, L"play_pause", L"Play/Pause", L"Space"},
    {Act::Play, L"play", L"Play", L""},
    {Act::Pause, L"pause", L"Pause", L""},
    {Act::Stop, L"stop", L"Stop", L"Period"},
    {Act::FrameStep, L"frame_step", L"Frame step", L"Ctrl+Right"},
    {Act::FrameBack, L"frame_back", L"Frame step back", L"Ctrl+Left"},
    {Act::SeekFwdSmall, L"seek_forward_small", L"Jump forward (small)", L""},
    {Act::SeekBackSmall, L"seek_back_small", L"Jump backward (small)", L""},
    {Act::SeekFwdMed, L"seek_forward", L"Jump forward (medium)", L"Right, MouseX2"},
    {Act::SeekBackMed, L"seek_back", L"Jump backward (medium)", L"Left, MouseX1"},
    {Act::SeekFwdLarge, L"seek_forward_large", L"Jump forward (large)", L"Alt+Right"},
    {Act::SeekBackLarge, L"seek_back_large", L"Jump backward (large)", L"Alt+Left"},
    {Act::KeyframeFwd, L"keyframe_forward", L"Jump forward (keyframe)", L"Shift+Right"},
    {Act::KeyframeBack, L"keyframe_back", L"Jump backward (keyframe)", L"Shift+Left"},
    {Act::SeekStart, L"seek_start", L"Go to start", L"Home"},
    {Act::GoTo, L"go_to", L"Go to...", L"Ctrl+G"},
    {Act::SpeedUp, L"speed_up", L"Increase rate", L"Ctrl+Up"},
    {Act::SpeedDown, L"speed_down", L"Decrease rate", L"Ctrl+Down"},
    {Act::SpeedReset, L"speed_reset", L"Reset rate", L"Ctrl+R"},
    {Act::AudioDelayUp, L"audio_delay_up", L"Audio delay +10 ms", L"NumPlus"},
    {Act::AudioDelayDown, L"audio_delay_down", L"Audio delay -10 ms", L"NumMinus"},
    {Act::SubDelayUp, L"subtitle_delay_up", L"Subtitle delay +", L"F2"},
    {Act::SubDelayDown, L"subtitle_delay_down", L"Subtitle delay -", L"F1"},
    {Act::NextChapter, L"next", L"Next (chapter or file)", L"PageDown"},
    {Act::PrevChapter, L"previous", L"Previous (chapter or file)", L"PageUp"},
    {Act::NextFile, L"next_file", L"Next file", L"Ctrl+PageDown"},
    {Act::PrevFile, L"previous_file", L"Previous file", L"Ctrl+PageUp"},
    {Act::Open, L"open", L"Open file...", L"Ctrl+O, Ctrl+Q"},
    {Act::Close, L"close", L"Close", L"Ctrl+C"},
    {Act::Exit, L"exit", L"Exit", L"Alt+X"},
    {Act::Fullscreen, L"fullscreen", L"Fullscreen", L"Alt+Enter, F11, MouseLeftDouble"},
    {Act::ExitFullscreen, L"exit_fullscreen", L"Exit fullscreen", L"Escape"},
    {Act::OnTop, L"on_top", L"Always on top", L"Ctrl+A"},
    {Act::ToggleCaption, L"toggle_caption", L"Hide caption", L"Ctrl+0"},
    {Act::ToggleControls, L"toggle_controls", L"Show controls", L"Ctrl+1, Ctrl+2"},
    {Act::ViewMinimal, L"view_minimal", L"Minimal view", L"1"},
    {Act::ViewNormal, L"view_normal", L"Normal view", L"3"},
    {Act::Zoom50, L"zoom_50", L"Zoom 50%", L"Alt+1"},
    {Act::Zoom100, L"zoom_100", L"Zoom 100%", L"Alt+2"},
    {Act::Zoom200, L"zoom_200", L"Zoom 200%", L"Alt+3"},
    {Act::ZoomAutofit, L"zoom_autofit", L"Auto fit", L"Alt+4"},
    {Act::PanLeft, L"pan_left", L"Pan left", L"Num4"},
    {Act::PanRight, L"pan_right", L"Pan right", L"Num6"},
    {Act::PanUp, L"pan_up", L"Pan up", L"Num8"},
    {Act::PanDown, L"pan_down", L"Pan down", L"Num2"},
    {Act::PanReset, L"pan_reset", L"Reset zoom and pan", L"Num5"},
    {Act::ZoomIn, L"zoom_in", L"Zoom in", L"Num9, Ctrl+WheelUp"},
    {Act::ZoomOut, L"zoom_out", L"Zoom out", L"Num1, Ctrl+WheelDown"},
    {Act::VolUp, L"volume_up", L"Volume up", L"Up, WheelUp"},
    {Act::VolDown, L"volume_down", L"Volume down", L"Down, WheelDown"},
    {Act::Mute, L"mute", L"Mute", L"Ctrl+M"},
    {Act::NextAudio, L"next_audio", L"Next audio track", L"A"},
    {Act::PrevAudio, L"previous_audio", L"Previous audio track", L"Shift+A"},
    {Act::NextSub, L"next_subtitle", L"Next subtitle", L"S"},
    {Act::PrevSub, L"previous_subtitle", L"Previous subtitle", L"Shift+S"},
    {Act::SubToggle, L"subtitles_on_off", L"Subtitles on/off", L"W"},
    {Act::ScreenshotSave, L"save_image", L"Save image...", L"Alt+I"},
    {Act::ScreenshotAuto, L"save_image_auto", L"Save image (auto)", L"F5"},
    {Act::AbStart, L"ab_start", L"Set A-B loop start", L"LBracket"},
    {Act::AbEnd, L"ab_end", L"Set A-B loop end", L"RBracket"},
    {Act::AbClear, L"ab_clear", L"Clear A-B loop", L"Backslash"},
    {Act::Stats, L"statistics", L"Statistics", L"Ctrl+4, Tab"},
    {Act::Properties, L"properties", L"Properties", L"Shift+F10"},
    {Act::EditSettings, L"options", L"Options (edit fastplay.ini)", L"O"},
    {Act::Menu, L"menu", L"Context menu", L"Apps"},
};

const ActionInfo* Actions(int* count) {
    *count = (int)std::size(kActions);
    return kActions;
}

const ActionInfo* FindAction(Act a) {
    for (const auto& i : kActions)
        if (i.act == a) return &i;
    return nullptr;
}

struct KeyName { uint32_t vk; const wchar_t* name; };
static const KeyName kNames[] = {
    {VK_SPACE, L"Space"}, {VK_RETURN, L"Enter"}, {VK_ESCAPE, L"Escape"}, {VK_TAB, L"Tab"},
    {VK_BACK, L"Backspace"}, {VK_DELETE, L"Delete"}, {VK_INSERT, L"Insert"}, {VK_HOME, L"Home"},
    {VK_END, L"End"}, {VK_PRIOR, L"PageUp"}, {VK_NEXT, L"PageDown"}, {VK_LEFT, L"Left"},
    {VK_RIGHT, L"Right"}, {VK_UP, L"Up"}, {VK_DOWN, L"Down"}, {VK_APPS, L"Apps"}, {VK_PAUSE, L"Pause"},
    {VK_ADD, L"NumPlus"}, {VK_SUBTRACT, L"NumMinus"}, {VK_MULTIPLY, L"NumMul"}, {VK_DIVIDE, L"NumDiv"},
    {VK_DECIMAL, L"NumDot"}, {VK_OEM_PERIOD, L"Period"}, {VK_OEM_COMMA, L"Comma"}, {VK_OEM_1, L"Semicolon"},
    {VK_OEM_7, L"Quote"}, {VK_OEM_2, L"Slash"}, {VK_OEM_5, L"Backslash"}, {VK_OEM_4, L"LBracket"},
    {VK_OEM_6, L"RBracket"}, {VK_OEM_3, L"Backtick"}, {VK_OEM_MINUS, L"Minus"}, {VK_OEM_PLUS, L"Equals"},
    {kWheelUp, L"WheelUp"}, {kWheelDown, L"WheelDown"}, {kWheelLeft, L"WheelLeft"}, {kWheelRight, L"WheelRight"},
    {kMouseMiddle, L"MouseMiddle"}, {kMouseX1, L"MouseX1"}, {kMouseX2, L"MouseX2"},
    {kMouseLeftDbl, L"MouseLeftDouble"},
};

uint32_t CurrentMods() {
    uint32_t m = 0;
    if (GetKeyState(VK_CONTROL) < 0) m |= kModCtrl;
    if (GetKeyState(VK_SHIFT) < 0) m |= kModShift;
    if (GetKeyState(VK_MENU) < 0) m |= kModAlt;
    return m;
}

static std::wstring KeyToName(uint32_t vk) {
    for (const auto& k : kNames)
        if (k.vk == vk) return k.name;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num" + std::to_wstring(vk - VK_NUMPAD0);
    wchar_t b[16];
    swprintf(b, 16, L"VK%02X", vk);
    return b;
}

static bool NameToKey(const std::wstring& n, uint32_t* vk) {
    std::wstring l = util::Lower(n);
    for (const auto& k : kNames)
        if (util::Lower(k.name) == l) { *vk = k.vk; return true; }
    if (l == L"esc") { *vk = VK_ESCAPE; return true; }
    if (l == L"return") { *vk = VK_RETURN; return true; }
    if (l.size() == 1 && ((l[0] >= L'a' && l[0] <= L'z') || (l[0] >= L'0' && l[0] <= L'9'))) {
        *vk = (uint32_t)towupper(l[0]);
        return true;
    }
    if (l.size() >= 2 && l[0] == L'f' && iswdigit(l[1])) {
        int n2 = _wtoi(l.c_str() + 1);
        if (n2 >= 1 && n2 <= 24) { *vk = VK_F1 + n2 - 1; return true; }
    }
    if (l.size() == 4 && l.compare(0, 3, L"num") == 0 && iswdigit(l[3])) { *vk = VK_NUMPAD0 + (l[3] - L'0'); return true; }
    if (l.size() == 4 && l.compare(0, 2, L"vk") == 0) { *vk = wcstoul(l.c_str() + 2, nullptr, 16); return true; }
    return false;
}

std::wstring ChordName(uint32_t chord) {
    std::wstring s;
    if (chord & kModCtrl) s += L"Ctrl+";
    if (chord & kModShift) s += L"Shift+";
    if (chord & kModAlt) s += L"Alt+";
    return s + KeyToName(chord & 0xFFFF);
}

bool ParseChord(const std::wstring& text, uint32_t* chord) {
    std::wstring t = util::Trim(text);
    if (t.empty()) return false;
    uint32_t mods = 0;
    size_t pos = 0;
    while (true) {
        size_t plus = t.find(L'+', pos);
        if (plus == std::wstring::npos) break;
        std::wstring m = util::Lower(util::Trim(t.substr(pos, plus - pos)));
        if (m == L"ctrl" || m == L"control") mods |= kModCtrl;
        else if (m == L"shift") mods |= kModShift;
        else if (m == L"alt") mods |= kModAlt;
        else return false;
        pos = plus + 1;
    }
    uint32_t vk;
    if (!NameToKey(util::Trim(t.substr(pos)), &vk)) return false;
    *chord = mods | vk;
    return true;
}

static void ForEachChord(const std::wstring& list, const std::function<void(uint32_t)>& fn) {
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(L',', pos);
        if (comma == std::wstring::npos) comma = list.size();
        uint32_t c;
        if (ParseChord(list.substr(pos, comma - pos), &c)) fn(c);
        pos = comma + 1;
    }
}

void KeyMap::Load(const Ini& ini) {
    map_.clear();
    text_.clear();
    for (const auto& a : kActions) {
        std::wstring list = ini.Has(L"keys", a.id) ? ini.Get(L"keys", a.id) : a.defaults;
        bool first = true;
        ForEachChord(list, [&](uint32_t c) {
            map_[c] = a.act;
            if (first) text_[(int)a.act] = ChordName(c);
            first = false;
        });
    }
}

void KeyMap::WriteDefaults(Ini& ini) const {
    for (const auto& a : kActions)
        if (!ini.Has(L"keys", a.id)) ini.Set(L"keys", a.id, a.defaults);
}

Act KeyMap::Lookup(uint32_t chord) const {
    auto it = map_.find(chord);
    return it == map_.end() ? Act::None : it->second;
}

std::wstring KeyMap::ShortcutText(Act a) const {
    auto it = text_.find((int)a);
    return it == text_.end() ? L"" : it->second;
}
